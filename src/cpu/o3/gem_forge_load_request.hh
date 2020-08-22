#ifndef __CPU_O3_GEM_FORGE_LOAD_REQUEST_HH__
#define __CPU_O3_GEM_FORGE_LOAD_REQUEST_HH__

#include "lsq.hh"

#include "cpu/gem_forge/gem_forge_lsq_callback.hh"

/**
 * ! GemForge
 * Used to implement special GemForgeLoadRequest.
 */
template <class Impl> class GemForgeLoadRequest : public LSQ<Impl>::LSQRequest {
public:
  /**
   * We have to explicit declare these names in template parent class.
   */
  using Flag = typename LSQ<Impl>::LSQRequest::Flag;
  using State = typename LSQ<Impl>::LSQRequest::State;
  using DynInstPtr = typename LSQ<Impl>::DynInstPtr;
  using LSQUnit = typename LSQ<Impl>::LSQUnit;
  using LSQSenderState = typename LSQ<Impl>::LSQSenderState;
  using O3CPUDelegator = typename Impl::CPUPol::O3CPUDelegator;

  GemForgeLoadRequest(LSQUnit *port, const DynInstPtr &inst,
                      O3CPUDelegator *_cpuDelegator,
                      GemForgeLQCallbackPtr _callback)
      : LSQ<Impl>::LSQRequest(port, inst, true /* isLoad */,
                              _callback->getAddr(), _callback->getSize(),
                              0 /* Flags */),
        cpuDelegator(_cpuDelegator), callback(std::move(_callback)),
        checkValueReadyEvent([this]() -> void { this->checkValueReady(); },
                             port->name()) {}

  ~GemForgeLoadRequest() override {}
  bool isGemForgeLoadRequest() const override { return true; }
  void initiateTranslation() override;
  void finish(const Fault &fault, const RequestPtr &req, ThreadContext *tc,
              BaseTLB::Mode mode) override {
    panic("GemForgeLoadRequest::finish should never be called.");
  }
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
  // The GemForgeLQCallback.
  GemForgeLQCallbackPtr callback;

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
#endif
