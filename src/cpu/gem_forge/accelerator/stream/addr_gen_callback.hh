#ifndef __GEM_FORGE_STREAM_ADDRESS_GENERATE_CALLBACK_HH__
#define __GEM_FORGE_STREAM_ADDRESS_GENERATE_CALLBACK_HH__

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct DynamicStreamFormalParam {
  union {
    uint64_t invariant;
    uint64_t baseStreamId;
  } param;
  bool isInvariant;
};

using DynamicStreamFormalParamV = std::vector<DynamicStreamFormalParam>;
using DynamicStreamParamV = std::vector<uint64_t>;
using GetStreamValueFunc = std::function<uint64_t(uint64_t)>;

/**
 * Used if you expect no base stream.
 */
uint64_t getStreamValueFail(uint64_t streamId);

/**
 * This represent a stateless addr_gen function.
 */
struct AddrGenCallback {
protected:
  virtual uint64_t genAddr(uint64_t idx, const DynamicStreamParamV &params) = 0;

  /**
   * This is a helper function to actually call the address callback.
   * Given a callback to collect base stream value.
   */
public:
  uint64_t genAddr(uint64_t idx, const DynamicStreamFormalParamV &formalParams,
                   GetStreamValueFunc getStreamValue);
};

using AddrGenCallbackPtr = std::shared_ptr<AddrGenCallback>;

struct LinearAddrGenCallback : public AddrGenCallback {
  uint64_t genAddr(uint64_t idx, const DynamicStreamParamV &params) override;

public:
  bool isContinuous(const DynamicStreamFormalParamV &params,
                    int32_t elementSize);
  /**
   * Only valid when continuous.
   */
  uint64_t getStartAddr(const DynamicStreamFormalParamV &params);
  /**
   * Compute the first elementIdx that will touch the address.
   */
  uint64_t getFirstElementForAddr(const DynamicStreamFormalParamV &params,
                                  int32_t elementSize, uint64_t addr);
};

#endif