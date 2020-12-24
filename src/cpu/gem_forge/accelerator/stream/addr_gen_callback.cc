#include "addr_gen_callback.hh"
#include "debug/AddrGenCallback.hh"

#include "base/logging.hh"
#include "base/trace.hh"

StreamValue getStreamValueFail(uint64_t streamId) {
  assert(false && "Failed to get stream value.");
}

StreamValue GetSingleStreamValue::operator()(uint64_t streamId) const {
  assert(this->streamId == streamId && "Invalid base stream.");
  return this->streamValue;
}

DynamicStreamParamV
convertFormalParamToParam(const DynamicStreamFormalParamV &formalParams,
                          GetStreamValueFunc getStreamValue) {
  DynamicStreamParamV params;
  for (const auto &formalParam : formalParams) {
    if (formalParam.isInvariant) {
      // Promote to StreamValue.
      StreamValue value{0};
      value.front() = formalParam.param.invariant;
      params.push_back(value);
    } else {
      auto baseStreamId = formalParam.param.baseStreamId;
      auto baseStreamValue = getStreamValue(baseStreamId);
      params.push_back(baseStreamValue);
    }
  }
  return params;
}

StreamValue
AddrGenCallback::genAddr(uint64_t idx,
                         const DynamicStreamFormalParamV &formalParams,
                         GetStreamValueFunc getStreamValue) {

  // 1. Prepare the parameters.
  auto params = convertFormalParamToParam(formalParams, getStreamValue);

  // 2. Call the AddrGenCallback.
  return this->genAddr(idx, params);
}

StreamValue LinearAddrGenCallback::genAddr(uint64_t idx,
                                           const DynamicStreamParamV &params) {
  /**
   * LINEAR pattern has 2n or (2n+1) parameters, where n is the difference of
   * loop level between ConfigureLoop and InnerMostLoop. It has the following
   * format, starting from InnerMostLoop.
   *
   * Stride0, [TotalTripCount[i], Stride[i + 1]]*, [TotalTripCount[n]], Start
   *
   * For address computation, we ignore the last optional TotalTripCount[n],
   * which if provided, should be the total length of this dynamic stream.
   *
   * Somehow the computation is like this.
   * Val[i] = Val[i - 1] + Stride * (idx / TotalTripCount[i])
   */
  assert(params.size() >= 2 && "Invalid number of inputs.");

#define getParam(i) params.at(i).front()

  auto start = getParam(params.size() - 1);
  auto strideStartIdx =
      (params.size() % 2 == 1) ? (params.size() - 3) : (params.size() - 2);
  auto stride = getParam(strideStartIdx);
  auto nestedIdx = idx;
  DPRINTF(AddrGenCallback,
          "[LinearAddrGen]: idx %llu, start %#x, stride %#x.\n", idx, start,
          stride);
  // ! Be careful to avoid underflow.
  for (auto paramIdx = strideStartIdx; paramIdx > 1; paramIdx -= 2) {
    auto totalTripCount = getParam(paramIdx - 1);
    auto newStart = start + stride * (nestedIdx / totalTripCount);
    auto newStride = getParam(paramIdx - 2);
    auto newIdx = nestedIdx % totalTripCount;
    start = newStart;
    stride = newStride;
    nestedIdx = newIdx;
    DPRINTF(AddrGenCallback,
            "[LinearAddrGen]: totalTripCount %llu, nestedIdx %llu, start %#x, "
            "stride %#x.\n",
            totalTripCount, nestedIdx, start, stride);
  }
  auto addr = start + stride * nestedIdx;
  DPRINTF(AddrGenCallback, "[LinearAddrGen]: Final addr %#x.\n", addr);

#undef getParam
  StreamValue retAddr{0};
  retAddr.front() = addr;
  return retAddr;
}

bool LinearAddrGenCallback::isContinuous(
    const DynamicStreamFormalParamV &params, int32_t elementSize) {
  assert(params.size() >= 2 && "Invalid number of inputs.");
  // Make sure it is all invariant.
  for (const auto &p : params) {
    if (!p.isInvariant) {
      return false;
    }
  }
  auto stride0 = params[0].param.invariant;
  if (stride0 > elementSize) {
    return false;
  }
  for (auto paramIdx = 1; paramIdx + 2 < params.size(); paramIdx += 2) {
    auto totalTripCount = params[paramIdx].param.invariant;
    auto newStride = params[paramIdx + 1].param.invariant;
    DPRINTF(AddrGenCallback,
            "[LinearAddrGen]: newStride %llu, totalTripCount %llu, stride0 "
            "%llu.\n",
            newStride, totalTripCount, stride0);
    if (stride0 * totalTripCount != newStride) {
      return false;
    }
  }
  return true;
}

int64_t
LinearAddrGenCallback::getInnerStride(const DynamicStreamFormalParamV &params) {
  auto idx = 0;
  assert(params.at(idx).isInvariant && "Variant inner stride.");
  return params.at(idx).param.invariant;
}

uint64_t
LinearAddrGenCallback::getStartAddr(const DynamicStreamFormalParamV &params) {
  // The last one is start address.
  return params.rbegin()->param.invariant;
}

uint64_t LinearAddrGenCallback::getFirstElementForAddr(
    const DynamicStreamFormalParamV &params, int32_t elementSize,
    uint64_t addr) {
  // Get the stride 0.
  auto startAddr = this->getStartAddr(params);
  assert(addr > startAddr + elementSize && "Addr too small.");
  auto stride0 = params.front().param.invariant;
  return (addr - startAddr) / stride0 + 1;
}

bool LinearAddrGenCallback::estimateReuse(
    const DynamicStreamFormalParamV &params, uint64_t elementSize,
    uint64_t &reuseFootprint, uint64_t &reuseCount) {
  assert(params.size() >= 2);
  for (const auto &param : params) {
    assert(param.isInvariant && "Variant param for LinearAddrGenCallback.");
  }
  auto hasTotalTripCount = params.size() % 2 == 1;
  // We search for 0 stride.
  auto strideEnd = hasTotalTripCount ? params.size() - 2 : params.size() - 1;

  // Reuse stride index is the first zero stride index, when we fall back to
  // StartAddr.
  int reuseStrideIdx = -1;
  for (int strideIdx = 0; strideIdx < strideEnd; ++strideIdx) {
    auto stride = params.at(strideIdx).param.invariant;
    if (stride == 0) {
      // We found 0 stride -- we are back to StartVAddr, so reuse happens.
      reuseStrideIdx = strideIdx;
      break;
    }
  }

  if (reuseStrideIdx == -1) {
    // No reuse found within this stream.
    return false;
  }

  /**
   * Try to estimate reuse footprint.
   * Adjust the elementSize by FirstStride.
   * Get the number of elements before reuse happens.
   */
  uint64_t adjustedElementSize =
      (reuseStrideIdx > 0)
          ? std::min(elementSize, static_cast<uint64_t>(
                                      std::abs(this->getInnerStride(params))))
          : elementSize;
  uint64_t numElementBeforeReuse =
      (reuseStrideIdx > 0) ? params.at(reuseStrideIdx - 1).param.invariant : 1;
  reuseFootprint = numElementBeforeReuse * adjustedElementSize;
  /**
   * Try to get minimal reuse count.
   */
  reuseCount = (reuseStrideIdx + 2 < params.size())
                   ? (params.at(reuseStrideIdx + 1).param.invariant /
                      numElementBeforeReuse)
                   : 1;
  return true;
}

uint64_t
LinearAddrGenCallback::getNestTripCount(const DynamicStreamFormalParamV &params,
                                        int nestLevel) {
  auto knownLevels = (params.size() - 1) / 2;
  assert(knownLevels >= nestLevel);
  assert(nestLevel > 0);
  uint64_t nestTripCount = params.at((nestLevel - 1) * 2 + 1).param.invariant;
  return nestTripCount;
}