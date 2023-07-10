#include "addr_gen_callback.hh"
#include "debug/AddrGenCallback.hh"

#include "base/logging.hh"
#include "base/trace.hh"

namespace gem5 {

bool isSameInvariantFormalParams(const DynStreamFormalParamV &paramsA,
                                 const DynStreamFormalParamV &paramsB) {
  if (paramsA.size() != paramsB.size()) {
    return false;
  }
  for (auto i = 0; i < paramsA.size(); ++i) {
    const auto &p1 = paramsA.at(i);
    const auto &p2 = paramsB.at(i);
    assert(p1.isInvariant && p2.isInvariant);
    if (p1.invariant != p2.invariant) {
      return false;
    }
  }
  return true;
}

StreamValue getStreamValueFail(uint64_t streamId) {
  panic("Failed to get stream value.");
}

std::ostream &operator<<(std::ostream &os,
                         const DynStreamFormalParamV &params) {
  for (const auto &p : params) {
    if (p.isInvariant) {
      os << "Const-" << p.invariant.print() << " ";
    } else {
      os << "Strm-" << p.baseStreamId << " ";
    }
  }
  return os;
}

std::string to_string(const DynStreamFormalParamV &params) {
  std::stringstream ss;
  ss << params;
  return ss.str();
}

std::string printAffinePatternParams(const DynStreamFormalParamV &params) {
  assert(!params.empty());
  std::stringstream ss;
  uint64_t prevTrip = 1;
  // Start.
  bool invalid = false;
  ss << std::hex << params.back().invariant.uint64() << std::dec;
  for (int i = 0; i + 1 < params.size(); i += 2) {
    // Stride.
    ss << ":" << params.at(i).invariant.int64();
    if (i + 2 < params.size()) {
      // Trip.
      auto trip = params.at(i + 1).invariant.uint64();
      if (prevTrip == 0 || trip == 0) {
        ss << ":" << trip;
      } else {
        if (trip < prevTrip) {
          invalid = true;
        }
        auto ratio = trip / prevTrip;
        auto remainder = trip % prevTrip;
        ss << ":" << ratio;
        if (remainder != 0) {
          ss << "%" << remainder;
        }
      }
      prevTrip = trip;
    }
  }
  if (invalid) {
    std::stringstream ss;
    ss << std::hex << params.back().invariant.uint64() << std::dec;
    for (int i = 0; i + 1 < params.size(); i += 2) {
      // Stride.
      ss << ":" << params.at(i).invariant.int64();
      if (i + 2 < params.size()) {
        // Trip.
        auto trip = params.at(i + 1).invariant.uint64();
        ss << ":" << trip;
        prevTrip = trip;
      }
    }
    panic("[InvalidAffinePattern] InvalidTrip %s.", ss.str());
  }
  return ss.str();
}

void extractStrideAndTripFromAffinePatternParams(
    const DynStreamFormalParamV &params, std::vector<int64_t> &strides,
    std::vector<int64_t> &trips) {
  strides.clear();
  trips.clear();
  assert((params.size() % 2) == 1);
  uint64_t prevTrip = 1;
  for (int i = 1; i + 1 < params.size(); i += 2) {
    const auto &p = params.at(i);
    assert(p.isInvariant);
    auto trip = p.invariant.uint64();
    trips.push_back(trip / prevTrip);
    prevTrip = trip;
    const auto &s = params.at(i - 1);
    assert(s.isInvariant);
    auto stride = s.invariant.uint64();
    strides.push_back(stride);
  }
  assert(!trips.empty());
}

DynStreamFormalParamV
constructFormalParamsFromStrideAndTrip(int64_t start,
                                       const std::vector<int64_t> &strides,
                                       const std::vector<int64_t> &trips) {

  DynStreamFormalParamV params(strides.size() * 2 + 1);

#define setTrip(dim, t)                                                        \
  {                                                                            \
    params.at((dim)*2 + 1).isInvariant = true;                                 \
    params.at((dim)*2 + 1).invariant.uint64() = t;                             \
  }
#define setStride(dim, t)                                                      \
  {                                                                            \
    params.at((dim)*2).isInvariant = true;                                     \
    params.at((dim)*2).invariant.uint64() = t;                                 \
  }
#define setStart(t)                                                            \
  {                                                                            \
    params.back().isInvariant = true;                                          \
    params.back().invariant.uint64() = t;                                      \
  }

  auto accTrip = 1;
  for (int i = 0; i < strides.size(); ++i) {
    setStride(i, strides.at(i));
    accTrip *= trips.at(i);
    setTrip(i, accTrip);
  }
  setStart(start);

  return params;
}

StreamValue GetSingleStreamValue::operator()(uint64_t streamId) const {
  assert(this->streamId == streamId && "Invalid base stream.");
  return this->streamValue;
}

DynStreamParamV
convertFormalParamToParam(const DynStreamFormalParamV &formalParams,
                          GetStreamValueFunc getStreamValue) {
  DynStreamParamV params;
  for (const auto &formalParam : formalParams) {
    if (formalParam.isInvariant) {
      params.push_back(formalParam.invariant);
    } else {
      auto baseStreamId = formalParam.baseStreamId;
      auto baseStreamValue = getStreamValue(baseStreamId);
      params.push_back(baseStreamValue);
    }
  }
  return params;
}

StreamValue AddrGenCallback::genAddr(uint64_t idx,
                                     const DynStreamFormalParamV &formalParams,
                                     GetStreamValueFunc getStreamValue) {

  // 1. Prepare the parameters.
  auto params = convertFormalParamToParam(formalParams, getStreamValue);

  // 2. Call the AddrGenCallback.
  return this->genAddr(idx, params);
}

StreamValue LinearAddrGenCallback::genAddr(uint64_t idx,
                                           const DynStreamParamV &params) {
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
    if (totalTripCount == 0) {
      auto newStart = start;
      auto newStride = getParam(paramIdx - 2);
      auto newIdx = nestedIdx;
      start = newStart;
      stride = newStride;
      nestedIdx = newIdx;
    } else {
      auto newStart = start + stride * (nestedIdx / totalTripCount);
      auto newStride = getParam(paramIdx - 2);
      auto newIdx = nestedIdx % totalTripCount;
      start = newStart;
      stride = newStride;
      nestedIdx = newIdx;
    }
    DPRINTF(AddrGenCallback,
            "[LinearAddrGen]: totalTripCount %llu, nestedIdx %llu, start %#x, "
            "stride %#x.\n",
            totalTripCount, nestedIdx, start, stride);
  }
  auto addr = start + stride * nestedIdx;
  DPRINTF(AddrGenCallback, "[LinearAddrGen]: Final addr %#x.\n", addr);

#undef getParam
  StreamValue retAddr;
  retAddr.front() = addr;
  return retAddr;
}

bool LinearAddrGenCallback::isContinuous(const DynStreamFormalParamV &params,
                                         int32_t elementSize) {
  assert(params.size() >= 2 && "Invalid number of inputs.");
  // Make sure it is all invariant.
  for (const auto &p : params) {
    if (!p.isInvariant) {
      return false;
    }
  }
  auto stride0 = params[0].invariant.uint64();
  if (stride0 > elementSize) {
    return false;
  }
  for (auto paramIdx = 1; paramIdx + 2 < params.size(); paramIdx += 2) {
    auto totalTripCount = params[paramIdx].invariant.uint64();
    auto newStride = params[paramIdx + 1].invariant.uint64();
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
LinearAddrGenCallback::getInnerStride(const DynStreamFormalParamV &params) {
  auto idx = 0;
  assert(params.at(idx).isInvariant && "Variant inner stride.");
  return params.at(idx).invariant.uint64();
}

uint64_t
LinearAddrGenCallback::getStartAddr(const DynStreamFormalParamV &params) {
  // The last one is start address.
  return params.rbegin()->invariant.uint64();
}

uint64_t LinearAddrGenCallback::getFirstElementForAddr(
    const DynStreamFormalParamV &params, int32_t elementSize, uint64_t addr) {
  // Get the stride 0.
  auto startAddr = this->getStartAddr(params);
  assert(addr > startAddr + elementSize && "Addr too small.");
  auto stride0 = params.front().invariant.uint64();
  return (addr - startAddr) / stride0 + 1;
}

int LinearAddrGenCallback::getFirstReuseDim(
    const DynStreamFormalParamV &params) {
  assert(params.size() >= 2);
  auto hasTotalTripCount = params.size() % 2 == 1;
  // We search for 0 stride.
  auto strideEnd = hasTotalTripCount ? params.size() - 2 : params.size() - 1;

  // Reuse stride index is the first zero stride index, when we fall back to
  // StartAddr.
  int reuseStrideIdx = -1;
  for (int strideIdx = 0; strideIdx < strideEnd; strideIdx += 2) {
    const auto &param = params.at(strideIdx);
    assert(param.isInvariant && "Variant param for LinearAddrGenCallback.");
    auto stride = param.invariant.uint64();
    if (stride == 0) {
      // We found 0 stride -- we are back to StartVAddr, so reuse happens.
      reuseStrideIdx = strideIdx / 2;
      break;
    }
  }
  return reuseStrideIdx;
}

bool LinearAddrGenCallback::estimateReuse(const DynStreamFormalParamV &params,
                                          uint64_t elementSize,
                                          uint64_t &reuseFootprint,
                                          uint64_t &reuseCount) {
  int reuseStrideIdx = this->getFirstReuseDim(params) * 2;

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
      (reuseStrideIdx > 0) ? params.at(reuseStrideIdx - 1).invariant.uint64()
                           : 1;
  reuseFootprint = numElementBeforeReuse * adjustedElementSize;
  /**
   * Try to get minimal reuse count.
   */
  reuseCount = (reuseStrideIdx + 2 < params.size())
                   ? (params.at(reuseStrideIdx + 1).invariant.uint64() /
                      numElementBeforeReuse)
                   : 1;
  return true;
}

uint64_t
LinearAddrGenCallback::getNestTripCount(const DynStreamFormalParamV &params,
                                        int nestLevel) {
  [[maybe_unused]] auto knownLevels = (params.size() - 1) / 2;
  assert(knownLevels >= nestLevel);
  assert(nestLevel > 0);
  uint64_t nestTripCount =
      params.at((nestLevel - 1) * 2 + 1).invariant.uint64();
  return nestTripCount;
}

} // namespace gem5
