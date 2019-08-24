#ifndef __MEM_RUBY_SLICC_INTERFACE_ABSTRACT_STREAM_AWARE_CONTROLLER_HH__
#define __MEM_RUBY_SLICC_INTERFACE_ABSTRACT_STREAM_AWARE_CONTROLLER_HH__

#include "AbstractController.hh"
#include "params/RubyStreamAwareController.hh"

/**
 * ! Sean: StreamAwareCache.
 * ! An abstract cache controller with stream information.
 */
class AbstractStreamAwareController : public AbstractController {
public:
  typedef RubyStreamAwareControllerParams Params;
  AbstractStreamAwareController(const Params *p);

  /**
   * Map an address to a LLC bank (or other type of controller).
   */
  MachineID mapAddressToLLC(Addr addr, MachineType mtype) const;

  bool isStreamFloatEnabled() const { return this->enableStreamFloat; }
  bool isStreamSublineEnabled() const { return this->enableStreamSubline; }

  /**
   * Set the hit cache level of the request.
   * TODO: Maybe move this into AbstractStreamAwareController.
   */
  void setHitCacheLevel(RequestStatisticPtr statistic,
                        int hitCacheLevel) const {
    if (statistic != nullptr) {
      statistic->setHitCacheLevel(hitCacheLevel);
    }
  }

  RequestStatisticPtr getRequestStatistic(PacketPtr pkt) const {
    if (pkt == nullptr) {
      return nullptr;
    }
    if (pkt->req == nullptr) {
      return nullptr;
    }
    if (!pkt->req->hasStatistic()) {
      return nullptr;
    }
    return pkt->req->getStatistic();
  }

private:
  /**
   * Store the bits used in S-NUCA to find the LLC bank.
   */
  const int llcSelectLowBit;
  const int llcSelectNumBits;
  const bool enableStreamFloat;
  const bool enableStreamSubline;
};

#endif