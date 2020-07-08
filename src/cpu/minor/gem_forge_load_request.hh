#ifndef __CPU_MINOR_GEM_FORGE_LOAD_REQUEST_HH__
#define __CPU_MINOR_GEM_FORGE_LOAD_REQUEST_HH__

#include "lsq.hh"

#include "cpu/gem_forge/gem_forge_lsq_callback.hh"

namespace Minor {

/**
 * Wraps GemForgeLQCallback into a LoadRequest into the LSQ.
 */
class GemForgeLoadRequest : public LSQ::LSQRequest {

public:
  GemForgeLQCallbackPtr callback;

  bool discarded = false;

protected:
  /**
   * Finish translation in TLB.
   */
  void finish(const Fault &fault, const RequestPtr &request, ThreadContext *tc,
              BaseTLB::Mode mode) override {}

public:
  /**
   * Start address translation. Currently do nothing.
   */
  void startAddrTranslation() override {}

  /**
   * Get the head packet to be sent.
   */
  PacketPtr getHeadPacket() override {
    panic("%s not implemented.", __PRETTY_FUNCTION__);
  }

  /**
   * Step to the next packet.
   */
  void stepToNextPacket() override {}

  /**
   * Any packet in memory system.
   */
  bool hasPacketsInMemSystem() override { return false; }

  /**
   * Have all packets been sent.
   */
  bool sentAllPackets() override { return true; }

  /**
   * Get the response packet.
   */
  void retireResponse(PacketPtr packet) override {}

  bool isGemForgeLoadRequest() const override { return true; }

  void checkIsComplete() override;

  /**
   * See MinorCPUDelegator::streamChange() for details.
   */
  void markDiscarded();

  GemForgeLoadRequest(LSQ &_port, MinorDynInstPtr _inst,
                      GemForgeLQCallbackPtr _callback)
      : LSQRequest(_port, _inst, true /* isLoad */),
        callback(std::move(_callback)) {
    // Make the GemForgeLoadRequest already issued.
    this->setState(LSQRequestState::RequestIssuing);
  }

  GemForgeLoadRequest(GemForgeLoadRequest &&other) = delete;
  GemForgeLoadRequest(const GemForgeLoadRequest &other) = delete;
  GemForgeLoadRequest &operator=(GemForgeLoadRequest &&other) = delete;
  GemForgeLoadRequest &operator=(const GemForgeLoadRequest &other) = delete;

  std::ostream &format(std::ostream &os) const override {
    return os << *(this->callback.get()) << "[State: " << this->state << ']';
  }
};
} // namespace Minor

#endif