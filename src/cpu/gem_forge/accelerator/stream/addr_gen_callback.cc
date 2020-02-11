#include "addr_gen_callback.hh"
#include "debug/AddrGenCallback.hh"

#include "base/logging.hh"
#include "base/trace.hh"

uint64_t getStreamValueFail(uint64_t streamId) {
  assert(false && "Failed to get stream value.");
}

uint64_t AddrGenCallback::genAddr(uint64_t idx,
                                  const DynamicStreamFormalParamV &formalParams,
                                  GetStreamValueFunc getStreamValue) {

  // 1. Prepare the parameters.
  DynamicStreamParamV params;
  for (const auto &formalParam : formalParams) {
    if (formalParam.isInvariant) {
      params.push_back(formalParam.param.invariant);
    } else {
      auto baseStreamId = formalParam.param.baseStreamId;
      auto baseStreamValue = getStreamValue(baseStreamId);
      params.push_back(baseStreamValue);
    }
  }

  // 2. Call the AddrGenCallback.
  return this->genAddr(idx, params);
}

uint64_t LinearAddrGenCallback::genAddr(uint64_t idx,
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

  auto start = params.at(params.size() - 1);
  auto strideStartIdx =
      (params.size() % 2 == 1) ? (params.size() - 3) : (params.size() - 2);
  auto stride = params.at(strideStartIdx);
  auto nestedIdx = idx;
  DPRINTF(AddrGenCallback,
          "[LinearAddrGen]: idx %llu, start %#x, stride %#x.\n", idx, start,
          stride);
  // ! Be careful to avoid underflow.
  for (auto paramIdx = strideStartIdx; paramIdx > 1; paramIdx -= 2) {
    auto totalTripCount = params.at(paramIdx - 1);
    auto newStart = start + stride * (nestedIdx / totalTripCount);
    auto newStride = params.at(paramIdx - 2);
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
  return addr;
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

uint64_t
LinearAddrGenCallback::getInnerStride(const DynamicStreamFormalParamV &params) {
  // auto idx = (params.size() % 2 == 0) ? params.size() - 2 : params.size() - 3;
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