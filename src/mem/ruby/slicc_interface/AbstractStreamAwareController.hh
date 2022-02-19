#ifndef __MEM_RUBY_SLICC_INTERFACE_ABSTRACT_STREAM_AWARE_CONTROLLER_HH__
#define __MEM_RUBY_SLICC_INTERFACE_ABSTRACT_STREAM_AWARE_CONTROLLER_HH__

#include "AbstractController.hh"
#include "cpu/gem_forge/accelerator/gem_forge_accelerator.hh"
#include "cpu/gem_forge/accelerator/stream/cache/DynStreamSliceIdVec.hh"
#include "mem/ruby/common/PCRequestRecorder.hh"
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

  void init() override;
  void regStats() override;

  /**
   * Get number of rows/cols for Mesh topology.
   * Only works for GarnetNetwork.
   */
  int getNumRows() const;
  int getNumCols() const;
  bool isMyNeighbor(MachineID machineId) const;

  /**
   * Map an address to a LLC bank or Mem Channel.
   */
  MachineID mapAddressToLLCOrMem(Addr addr, MachineType mtype) const;

  /**
   * Get a physical line address that map to our LLC bank.
   */
  Addr getAddressToOurLLC() const;

  int getNumCoresPerRow() const { return this->numCoresPerRow; }
  bool isStreamFloatEnabled() const { return this->enableStreamFloat; }
  bool isStreamSublineEnabled() const { return this->enableStreamSubline; }
  bool isStreamIdeaAckEnabled() const { return this->enableStreamIdeaAck; }
  bool isStreamIdeaMLCPopCheckEnabled() const {
    return myParams->enable_mlc_stream_idea_pop_check_llc_progress;
  }
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
  int getLLCStreamEngineIssueWidth() const {
    return this->myParams->llc_stream_engine_issue_width;
  }
  int getLLCStreamEngineComputeWidth() const {
    return this->myParams->llc_stream_engine_compute_width;
  }
  bool isLLCStreamEngineZeroComputeLatencyEnabled() const {
    return this->myParams->enable_llc_stream_zero_compute_latency;
  }
  int getLLCStreamEngineMigrateWidth() const {
    return this->myParams->llc_stream_engine_migrate_width;
  }
  int getLLCStreamMaxInflyRequest() const {
    return this->myParams->llc_stream_max_infly_request;
  }
  bool isStreamRangeSyncEnabled() const {
    return this->myParams->enable_stream_range_sync;
  }
  bool isStreamAtomicLockEnabled() const {
    return this->myParams->stream_atomic_lock_type != "none";
  }
  const std::string &getStreamAtomicLockType() const {
    return this->myParams->stream_atomic_lock_type;
  }
  bool isStreamFloatMemEnabled() const {
    return this->myParams->enable_stream_float_mem;
  }

  const char *getMachineTypeString() const {
    auto type = this->getMachineID().type;
    switch (type) {
    case MachineType::MachineType_L2Cache:
      return "LLC";
    case MachineType::MachineType_Directory:
      return "MEM";
    default:
      return "XXX";
    }
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
  int getMLCStreamBufferToSegmentRatio() const {
    return this->myParams->mlc_stream_buffer_to_segment_ratio;
  }

  int getMulticastGroupId(int coreId, int groupSize) const {
    /**
     * MulticastGroup is used for LLC to try to multicast streams from
     * cores within the same MulticastGroup. It's just a block in the
     * Mesh topology.
     */
    if (groupSize == 0) {
      // Just one large MulticastGroup.
      return 0;
    }
    auto groupPerRow = (this->numCoresPerRow + groupSize - 1) / groupSize;
    auto rowId = coreId / this->numCoresPerRow;
    auto colId = coreId % this->numCoresPerRow;
    auto multicastGroupRowId = rowId / groupSize;
    auto multicastGroupColId = colId / groupSize;
    return multicastGroupRowId * groupPerRow + multicastGroupColId;
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
    case 16:
      return MessageSizeType_Response_Data_16B;
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

  void recordPCReq(const RequestStatisticPtr &reqStat) const;
  void recordDeallocateNoReuseReqStats(const RequestStatisticPtr &reqStat,
                                       CacheMemory &cache) const;
  void recordLLCReqQueueStats(const RequestStatisticPtr &reqStat,
                              const DynStreamSliceIdVec &sliceIds,
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

  const Params *myParams;

private:
  BaseCPU *cpu = nullptr;

  mutable PCRequestRecorder pcReqRecorder;

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

public:
  Stats::Distribution m_statLLCNumDirectStreams;
  // Stats for stream computing.
  Stats::Scalar m_statLLCScheduledComputation;
  Stats::Scalar m_statLLCScheduledComputeMicroOps;
  Stats::Scalar m_statLLCScheduledAffineLoadComputeMicroOps;
  Stats::Scalar m_statLLCScheduledAffineReduceMicroOps;
  Stats::Scalar m_statLLCScheduledAffineUpdateMicroOps;
  Stats::Scalar m_statLLCScheduledAffineStoreComputeMicroOps;
  Stats::Scalar m_statLLCScheduledAffineAtomicComputeMicroOps;
  Stats::Scalar m_statLLCScheduledIndirectLoadComputeMicroOps;
  Stats::Scalar m_statLLCScheduledIndirectReduceMicroOps;
  Stats::Scalar m_statLLCScheduledIndirectUpdateMicroOps;
  Stats::Scalar m_statLLCScheduledIndirectStoreComputeMicroOps;
  Stats::Scalar m_statLLCScheduledIndirectAtomicComputeMicroOps;
  Stats::Scalar m_statLLCScheduledPointerChaseLoadComputeMicroOps;
  Stats::Scalar m_statLLCScheduledPointerChaseReduceMicroOps;
  Stats::Scalar m_statLLCScheduledPointerChaseUpdateMicroOps;
  Stats::Scalar m_statLLCScheduledPointerChaseStoreComputeMicroOps;
  Stats::Scalar m_statLLCScheduledPointerChaseAtomicComputeMicroOps;
  Stats::Scalar m_statLLCScheduledMultiAffineLoadComputeMicroOps;
  Stats::Scalar m_statLLCScheduledMultiAffineReduceMicroOps;
  Stats::Scalar m_statLLCScheduledMultiAffineUpdateMicroOps;
  Stats::Scalar m_statLLCScheduledMultiAffineStoreComputeMicroOps;
  Stats::Scalar m_statLLCScheduledMultiAffineAtomicComputeMicroOps;

  Stats::Scalar m_statLLCPerformedAtomics;
  Stats::Scalar m_statLLCCommittedAtomics;
  Stats::Scalar m_statLLCLockedAtomics;
  Stats::Scalar m_statLLCUnlockedAtomics;
  Stats::Scalar m_statLLCLineConflictAtomics;
  Stats::Scalar m_statLLCRealConflictAtomics;
  Stats::Scalar m_statLLCXAWConflictAtomics;
  Stats::Scalar m_statLLCRealXAWConflictAtomics;
  Stats::Scalar m_statLLCDeadlockAtomics;
  Stats::Distribution m_statLLCNumInflyComputations;
  Stats::Distribution m_statLLCNumReadyComputations;
};

#endif