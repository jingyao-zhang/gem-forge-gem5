#ifndef __CPU_O3_GEM_FORGE_LOAD_REQUEST_HH__
#define __CPU_O3_GEM_FORGE_LOAD_REQUEST_HH__

#include "lsq.hh"
#include "dyn_inst.hh"

#include "cpu/gem_forge/gem_forge_lsq_callback.hh"

/**
 * ! GemForge
 * Used to implement special GemForgeLoadRequest.
 * ! This is abused to implement GemForgeAtomic.
 */

namespace gem5 {

class GemForgeLoadRequest : public o3::LSQ::LSQRequest {
public:
  /**
   * We have to explicit declare these names in template parent class.
   */
  using Flag = o3::LSQ::LSQRequest::Flag;
  using State = o3::LSQ::LSQRequest::State;
  using DynInstPtr = o3::DynInstPtr;
  using LSQUnit = o3::LSQUnit;
  using LSQSenderState = o3::LSQ::LSQRequest;

  GemForgeLoadRequest(LSQUnit *port, const DynInstPtr &inst,
                      O3CPUDelegator *_cpuDelegator,
                      GemForgeLSQCallbackPtr _callback)
      : o3::LSQ::LSQRequest(port, inst, inst->isLoad() /* isLoad */,
                            _callback->getAddr(), _callback->getSize(),
                            inst->isAtomic() ? Request::ATOMIC_RETURN_OP
                                             : Request::Flags(0) /* Flags */),
        cpuDelegator(_cpuDelegator), callback(std::move(_callback)),
        checkValueReadyEvent([this]() -> void { this->checkValueReady(); },
                             port->name()) {}

  ~GemForgeLoadRequest() override {}
  bool isGemForgeLoadRequest() const override { return true; }
  void initiateTranslation() override;
  void finish(const Fault &fault, const RequestPtr &req, ThreadContext *tc,
              BaseMMU::Mode mode) override {
    panic("GemForgeLoadRequest::finish should never be called.");
  }
  void markAsStaleTranslation() override;
  void release(Flag reason) override;
  bool recvTimingResp(PacketPtr pkt) override;
  void sendPacketToCache() override;
  void buildPackets() override;
  Cycles handleLocalAccess(ThreadContext *thread, PacketPtr pkt) override;
  bool isCacheBlockHit(Addr blockAddr, Addr cacheBlockMask) override;

  void squashInGemForge();

  void foundRAWMisspeculation();
  bool hasRAWMisspeculated() const { return this->rawMisspeculated; }

  bool bypassAliasCheck() const { return this->callback->bypassAliasCheck(); }
  bool hasOverlap(Addr vaddr, int size) const;
  bool hasNonCoreDependent() const {
    return this->callback->hasNonCoreDependent();
  }

protected:
  O3CPUDelegator *cpuDelegator;
  // The GemForgeLSQCallback.
  GemForgeLSQCallbackPtr callback;

  /**
   * O3 CPU is async, while GemForge requires me check for value ready.
   */
  EventFunctionWrapper checkValueReadyEvent;
  void checkValueReady();

  /**
   * ROB has a squash width, while GemForge will squash immediately.
   * This flag remembers that the instruction is squashed in GemForge.
   */
  bool squashedInGemForge = false;

  /**
   * This load request has triggered RAWMisspeculated. It should be
   * squashed in O3, but ROB has a squash width, and may still trying
   * to consult the callback about address/size, even in the same
   * cycle.
   * We remember this flag and O3CPUDelegator should considered me
   * like squashed.
   */
  bool rawMisspeculated = false;
};
} // namespace gem5
#endif
