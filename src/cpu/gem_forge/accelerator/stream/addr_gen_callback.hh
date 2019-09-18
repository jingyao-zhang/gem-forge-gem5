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
    assert(params.size() == 2 && "Invalid number of inputs.");
    auto base = params.at(0);
    auto stride = params.at(1);
    return base + idx * stride;
  }
};

#endif