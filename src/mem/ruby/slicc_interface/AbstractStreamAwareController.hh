#ifndef __MEM_RUBY_SLICC_INTERFACE_ABSTRACT_STREAM_AWARE_CONTROLLER_HH__
#define __MEM_RUBY_SLICC_INTERFACE_ABSTRACT_STREAM_AWARE_CONTROLLER_HH__

#include "AbstractController.hh"
#include "cpu/gem_forge/accelerator/gem_forge_accelerator.hh"
#include "cpu/gem_forge/accelerator/stream/cache/DynamicStreamSliceIdVec.hh"
#include "mem/ruby/structures/CacheMemory.hh"
#include "params/RubyStreamAwareController.hh"

/**
 * ! Sean: StreamAwareCache.
 * ! An abstract cache controller with stream information.
 */

class MLCStreamEngine;
class LLCStreamEngine;

class AbstractStreamAwareController : public AbstractController {
public:
  typedef RubyStreamAwareControllerParams Params;
  AbstractStreamAwareController(const Params *p);

  void regStats() override;

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
  bool isStreamIdeaAckEnabled() const { return this->enableStreamIdeaAck; }
  bool isStreamIdeaFlowEnabled() const {
    return myParams->enable_stream_idea_flow;
  }
  bool isStreamIdeaStoreEnabled() const { return this->enableStreamIdeaStore; }
  bool isStreamCompactStoreEnabled() const {
    return myParams->enable_stream_compact_store;
  }
  bool isStreamAdvanceMigrateEnabled() const {
    return this->enableStreamAdvanceMigrate;
  }
  bool isStreamMulticastEnabled() const { return this->enableStreamMulticast; }
  bool isStreamLLCIssueClearEnabled() const {
    return myParams->enable_stream_llc_issue_clear;
  }
  int getStreamMulticastGroupSize() const {
    return this->streamMulticastGroupSize;
  }
  int getLLCStreamEngineIssueWidth() const {
    return this->myParams->llc_stream_engine_issue_width;
  }
  int getLLCStreamEngineMigrateWidth() const {
    return this->myParams->llc_stream_engine_migrate_width;
  }
  int getLLCStreamMaxInflyRequest() const {
    return this->myParams->llc_stream_max_infly_request;
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

  void recordDeallocateNoReuseReqStats(const RequestStatisticPtr &reqStat,
                                       CacheMemory &cache) const;
  void recordLLCReqQueueStats(const RequestStatisticPtr &reqStat,
                              const DynamicStreamSliceIdVec &sliceIds,
                              bool isLoad);
  void incrementLLCIndReqQueueStats() { this->m_statLLCIndStreamReq++; }

  void addNoCControlMsgs(RequestStatisticPtr statistic, int msgs) const {
    if (statistic != nullptr) {
      statistic->addNoCControlMessages(msgs);
    }
  }
  void addNoCControlEvictMsgs(RequestStatisticPtr statistic, int msgs) const {
    if (statistic != nullptr) {
      statistic->addNoCControlEvictMessages(msgs);
    }
  }
  void addNoCDataMsgs(RequestStatisticPtr statistic, int msgs) const {
    if (statistic != nullptr) {
      statistic->addNoCDataMessages(msgs);
    }
  }

  bool isRequestStatisticStream(const RequestStatisticPtr &reqStat) const {
    return reqStat ? reqStat->isStream : false;
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

  void addToStat(Stats::Scalar &s, int v) const { s += v; }

  GemForgeCPUDelegator *getCPUDelegator();

  static AbstractStreamAwareController *getController(MachineID machineId);

  void registerMLCStreamEngine(MLCStreamEngine *mlc) {
    assert(!this->mlcSE);
    this->mlcSE = mlc;
  }
  MLCStreamEngine *getMLCStreamEngine() {
    assert(this->mlcSE);
    return this->mlcSE;
  }

  void registerLLCStreamEngine(LLCStreamEngine *llc) {
    assert(!this->llcSE);
    this->llcSE = llc;
  }
  LLCStreamEngine *getLLCStreamEngine() {
    assert(this->llcSE);
    return this->llcSE;
  }

private:
  const Params *myParams;
  BaseCPU *cpu = nullptr;
  /**
   * Store the bits used in S-NUCA to find the LLC bank.
   */
  const int llcSelectLowBit;
  const int llcSelectNumBits;
  const int numCoresPerRow;
  const bool enableStreamFloat;
  const bool enableStreamSubline;
  const bool enableStreamIdeaAck;
  const bool enableStreamIdeaStore;
  const bool enableStreamAdvanceMigrate;
  const bool enableStreamMulticast;
  const int streamMulticastGroupSize;
  int streamMulticastGroupPerRow;
  MulticastIssuePolicy streamMulticastIssuePolicy;
  const int mlcStreamBufferInitNumEntries;

  MLCStreamEngine *mlcSE = nullptr;
  LLCStreamEngine *llcSE = nullptr;

  /**
   * Get the global StreamAwareCacheController map.
   */
  using GlobalMap =
      std::map<MachineType, std::map<NodeID, AbstractStreamAwareController *>>;
  static GlobalMap globalMap;
  static std::list<AbstractStreamAwareController *> globalList;
  static void registerController(AbstractStreamAwareController *controller);

protected:
  // Stats exposed to the controller.
  Stats::Scalar m_statCoreReq;
  Stats::Scalar m_statCoreLoadReq;
  Stats::Scalar m_statCoreStreamReq;
  Stats::Scalar m_statCoreStreamLoadReq;
  Stats::Scalar m_statLLCStreamReq;
  Stats::Scalar m_statLLCIndStreamReq;
  Stats::Scalar m_statLLCMulticastStreamReq;
};

#endif