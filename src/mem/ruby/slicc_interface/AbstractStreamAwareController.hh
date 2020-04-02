#ifndef __MEM_RUBY_SLICC_INTERFACE_ABSTRACT_STREAM_AWARE_CONTROLLER_HH__
#define __MEM_RUBY_SLICC_INTERFACE_ABSTRACT_STREAM_AWARE_CONTROLLER_HH__

#include "AbstractController.hh"
#include "cpu/gem_forge/accelerator/gem_forge_accelerator.hh"
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

  /**
   * Get a physical line address that map to our LLC bank.
   */
  Addr getAddressToOurLLC() const;

  int getNumCoresPerRow() const { return this->numCoresPerRow; }
  bool isStreamFloatEnabled() const { return this->enableStreamFloat; }
  bool isStreamSublineEnabled() const { return this->enableStreamSubline; }
  bool isStreamMulticastEnabled() const { return this->enableStreamMulticast; }
  int getStreamMulticastGroupSize() const {
    return this->streamMulticastGroupSize;
  }

  /**
   * Issue policy, starting from the most relaxed to most conservative.
   * See LLCStreamEngine::canIssueByMulticastPolicy for explaination.
   */
  enum MulticastIssuePolicy {
    Any,
    FirstAllocated,
    First,
  };
  MulticastIssuePolicy getStreamMulticastIssuePolicy() const {
    return this->streamMulticastIssuePolicy;
  }
  int getMLCStreamBufferInitNumEntries() const {
    return this->mlcStreamBufferInitNumEntries;
  }

  int getMulticastGroupId(int coreId) const {
    /**
     * MulticastGroup is used for LLC to try to multicast streams from
     * cores within the same MulticastGroup. It's just a block in the
     * Mesh topology.
     */
    if (this->streamMulticastGroupSize == 0) {
      // Just one large MulticastGroup.
      return 0;
    }
    auto rowId = coreId / this->numCoresPerRow;
    auto colId = coreId % this->numCoresPerRow;
    auto multicastGroupRowId = rowId / this->streamMulticastGroupSize;
    auto multicastGroupColId = colId / this->streamMulticastGroupSize;
    auto multicastGroupPerRow = this->streamMulticastGroupPerRow;
    return multicastGroupRowId * multicastGroupPerRow + multicastGroupColId;
  }

  MessageSizeType getMessageSizeType(int size) const {
    assert(this->isStreamSublineEnabled());
    switch (size) {
    case 1:
      return MessageSizeType_Response_Data_1B;
    case 2:
      return MessageSizeType_Response_Data_2B;
    case 4:
      return MessageSizeType_Response_Data_4B;
    case 8:
      return MessageSizeType_Response_Data_8B;
    default:
      return MessageSizeType_Response_Data;
    }
  }

  /**
   * Set the hit cache level of the request.
   */
  void setHitCacheLevel(RequestStatisticPtr statistic,
                        int hitCacheLevel) const {
    if (statistic != nullptr) {
      statistic->setHitCacheLevel(hitCacheLevel);
    }
  }

  void addNoCControlMsgs(RequestStatisticPtr statistic, int msgs) const {
    if (statistic != nullptr) {
      statistic->addNoCControlMessages(msgs);
    }
  }
  int getNoCControlMsgs(const RequestStatisticPtr &statistic) const {
    return statistic ? statistic->nocControlMessages : 0;
  }

  void addNoCDataMsgs(RequestStatisticPtr statistic, int msgs) const {
    if (statistic != nullptr) {
      statistic->addNoCDataMessages(msgs);
    }
  }
  int getNoCDataMsgs(const RequestStatisticPtr &statistic) const {
    return statistic ? statistic->nocDataMessages : 0;
  }

  bool isRequestStatisticValid(const RequestStatisticPtr &reqStat) const {
    return reqStat != nullptr;
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

  void addToStat(Stats::Scalar &s, int v) const {
    s += v;
  }

  GemForgeCPUDelegator *getCPUDelegator();

private:
  /**
   * Store the bits used in S-NUCA to find the LLC bank.
   */
  BaseCPU *cpu = nullptr;
  const int llcSelectLowBit;
  const int llcSelectNumBits;
  const int numCoresPerRow;
  const bool enableStreamFloat;
  const bool enableStreamSubline;
  const bool enableStreamMulticast;
  const int streamMulticastGroupSize;
  int streamMulticastGroupPerRow;
  MulticastIssuePolicy streamMulticastIssuePolicy;
  const int mlcStreamBufferInitNumEntries;
};

#endif