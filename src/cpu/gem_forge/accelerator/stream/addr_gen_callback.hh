#ifndef __GEM_FORGE_STREAM_ADDRESS_GENERATE_CALLBACK_HH__
#define __GEM_FORGE_STREAM_ADDRESS_GENERATE_CALLBACK_HH__

#include <cstdint>
#include <vector>

struct AddrGenCallback {
  virtual uint64_t genAddr(uint64_t idx,
                           const std::vector<uint64_t> &params) = 0;
};

struct LinearAddrGenCallback : public AddrGenCallback {
  uint64_t genAddr(uint64_t idx, const std::vector<uint64_t> &params) override {
    /**
     * LINEAR pattern has 2n parameters, where n is the difference of loop
     * level between ConfigureLoop and InnerMostLoop.
     * It has the following format, starting from InnerMostLoop.
     * Stride0, [TotalTripCount[i], Stride[i + 1]]*, Start
     *
     * Somehow the computation is like this.
     * Val[i] = Val[i - 1] + Stride * (idx / TotalTripCount[i])
     */
    assert(params.size() >= 2 && params.size() % 2 == 0 &&
           "Invalid number of inputs.");
    auto start = params.at(params.size() - 1);
    auto stride = params.at(params.size() - 2);
    // ! Be careful to avoid underflow.
    for (auto paramIdx = params.size() - 2; paramIdx > 1; paramIdx -= 2) {
      auto totalTripCount = params.at(paramIdx - 1);
      auto newStart = start + stride * (idx / totalTripCount);
      auto newStride = params.at(paramIdx - 2);
      auto newIdx = idx % totalTripCount;
      start = newStart;
      stride = newStride;
      idx = newIdx;
    }
    return start + stride * idx;
  }
};

#endif