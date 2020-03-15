#ifndef __GEM_FORGE_STREAM_ADDRESS_GENERATE_CALLBACK_HH__
#define __GEM_FORGE_STREAM_ADDRESS_GENERATE_CALLBACK_HH__

#include "cpu/gem_forge/accelerator/arch/exec_func.hh"

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
using ExecFuncPtr = std::shared_ptr<TheISA::ExecFunc>;

/**
 * Use if you expect no base stream.
 */
uint64_t getStreamValueFail(uint64_t streamId);

/**
 * Use if you expect only one base stream.
 */
struct GetSingleStreamValue {
  const uint64_t streamId;
  const uint64_t streamValue;
  GetSingleStreamValue(uint64_t _streamId, uint64_t _streamValue)
      : streamId(_streamId), streamValue(_streamValue) {}
  uint64_t operator()(uint64_t streamId) const;
};

DynamicStreamParamV
convertFormalParamToParam(const DynamicStreamFormalParamV &formalParams,
                          GetStreamValueFunc getStreamValue);

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
   * Get the inner most stride.
   */
  uint64_t getInnerStride(const DynamicStreamFormalParamV &params);
  /**
   * Only valid when continuous.
   */
  uint64_t getStartAddr(const DynamicStreamFormalParamV &params);
  /**
   * Compute the first elementIdx that will touch the address.
   */
  uint64_t getFirstElementForAddr(const DynamicStreamFormalParamV &params,
                                  int32_t elementSize, uint64_t addr);
  /**
   * Estimate memory footprint and reuse count.
   * @return success, reuse footprint, reuse count.
   */
  bool estimateReuse(const DynamicStreamFormalParamV &params,
                     uint64_t elementSize, uint64_t &reuseFootprint,
                     uint64_t &reuseCount);
};

class FuncAddrGenCallback : public AddrGenCallback {
public:
  FuncAddrGenCallback(ExecFuncPtr _execFunc) : execFunc(_execFunc) {}

  uint64_t genAddr(uint64_t idx, const std::vector<uint64_t> &params) override {
    // We ignore the idx.
    return this->execFunc->invoke(params);
  }

private:
  ExecFuncPtr execFunc;
};

#endif