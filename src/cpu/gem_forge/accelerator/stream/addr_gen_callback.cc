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
                                        const std::vector<uint64_t> &params) {
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
