#ifndef __MEM_RUBY_SLICC_INTERFACE_ABSTRACT_STREAM_AWARE_CONTROLLER_HH__
#define __MEM_RUBY_SLICC_INTERFACE_ABSTRACT_STREAM_AWARE_CONTROLLER_HH__

#include "AbstractController.hh"
#include "cpu/gem_forge/accelerator/gem_forge_accelerator.hh"
#include "cpu/gem_forge/accelerator/stream/cache/DynStreamSliceIdVec.hh"
#include "mem/ruby/common/PCRequestRecorder.hh"
#include "mem/ruby/structures/CacheMemory.hh"
#include "params/RubyStreamAwareController.hh"

namespace gem5 {

/**
 * ! Sean: StreamAwareCache.
 * ! An abstract cache controller with stream information.
 */

class MLCStreamEngine;
class LLCStreamEngine;
class PUMTransposeUnit;

namespace ruby {

class AbstractStreamAwareController : public AbstractController {
public:
  PARAMS(RubyStreamAwareController)
  AbstractStreamAwareController(const Params &p);
  ~AbstractStreamAwareController();

  void init() override;
  void resetStats() override;
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

  /**
   * @brief Adjust the response latency of the cache.
   * Currently, this is used to charge extra latency to access cache lines
   * in transposed layout in in-memory computing.
   *
   * @return Cycles.
   */
  Cycles adjustResponseLat(Cycles responseLat, Addr paddr);

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
                              const DynStreamSliceIdVec &sliceIds, bool isLoad);
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

  void addToStat(statistics::Scalar &s, int v) const { s += v; }

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

  /**
   * An AdHoc interface to evict a clean cache line.
   * Used in PUM to override a line.
   */
  virtual void evictCleanLine(const Addr &paddrLine);

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

  PUMTransposeUnit *pumTransposUnit = nullptr;

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
  statistics::Scalar m_statCoreReq;
  statistics::Scalar m_statCoreLoadReq;
  statistics::Scalar m_statCoreStreamReq;
  statistics::Scalar m_statCoreStreamLoadReq;
  statistics::Scalar m_statLLCStreamReq;
  statistics::Scalar m_statLLCIndStreamReq;
  statistics::Scalar m_statLLCMulticastStreamReq;

public:
  Stats::Distribution m_statLLCNumDirectStreams;
  // Stats for cycles when streams are offloaded.
  statistics::Scalar m_statMLCStreamCycles;
  // Stats for stream computing.
  statistics::Scalar m_statLLCScheduledComputation;
  statistics::Scalar m_statLLCScheduledComputeMicroOps;
  statistics::Scalar m_statLLCScheduledAffineLoadComputeMicroOps;
  statistics::Scalar m_statLLCScheduledAffineReduceMicroOps;
  statistics::Scalar m_statLLCScheduledAffineUpdateMicroOps;
  statistics::Scalar m_statLLCScheduledAffineStoreComputeMicroOps;
  statistics::Scalar m_statLLCScheduledAffineAtomicComputeMicroOps;
  statistics::Scalar m_statLLCScheduledIndirectLoadComputeMicroOps;
  statistics::Scalar m_statLLCScheduledIndirectReduceMicroOps;
  statistics::Scalar m_statLLCScheduledIndirectUpdateMicroOps;
  statistics::Scalar m_statLLCScheduledIndirectStoreComputeMicroOps;
  statistics::Scalar m_statLLCScheduledIndirectAtomicComputeMicroOps;
  statistics::Scalar m_statLLCScheduledPointerChaseLoadComputeMicroOps;
  statistics::Scalar m_statLLCScheduledPointerChaseReduceMicroOps;
  statistics::Scalar m_statLLCScheduledPointerChaseUpdateMicroOps;
  statistics::Scalar m_statLLCScheduledPointerChaseStoreComputeMicroOps;
  statistics::Scalar m_statLLCScheduledPointerChaseAtomicComputeMicroOps;
  statistics::Scalar m_statLLCScheduledMultiAffineLoadComputeMicroOps;
  statistics::Scalar m_statLLCScheduledMultiAffineReduceMicroOps;
  statistics::Scalar m_statLLCScheduledMultiAffineUpdateMicroOps;
  statistics::Scalar m_statLLCScheduledMultiAffineStoreComputeMicroOps;
  statistics::Scalar m_statLLCScheduledMultiAffineAtomicComputeMicroOps;

  statistics::Scalar m_statLLCPerformedAtomics;
  statistics::Scalar m_statLLCCommittedAtomics;
  statistics::Scalar m_statLLCLockedAtomics;
  statistics::Scalar m_statLLCUnlockedAtomics;
  statistics::Scalar m_statLLCLineConflictAtomics;
  statistics::Scalar m_statLLCRealConflictAtomics;
  statistics::Scalar m_statLLCXAWConflictAtomics;
  statistics::Scalar m_statLLCRealXAWConflictAtomics;
  statistics::Scalar m_statLLCDeadlockAtomics;
  Stats::Distribution m_statLLCNumInflyComputations;
  Stats::Distribution m_statLLCNumReadyComputations;

  statistics::Scalar m_statPUMTotalCycles;
  statistics::Scalar m_statPUMPrefetchCycles;
  statistics::Scalar m_statPUMCompileCycles;
  statistics::Scalar m_statPUMComputeReadBits;
  statistics::Scalar m_statPUMComputeWriteBits;
  statistics::Scalar m_statPUMComputeCycles;
  statistics::Scalar m_statPUMDataMoveCycles;
  statistics::Scalar m_statPUMReduceCycles;
  statistics::Scalar m_statPUMMixCycles;
  statistics::Scalar m_statPUMComputeCmds;
  statistics::Scalar m_statPUMComputeOps;
  statistics::Scalar m_statPUMInterArrayCmds;
  statistics::Scalar m_statPUMIntraArrayCmds;
  statistics::Scalar m_statPUMSyncCmds;
  statistics::Scalar m_statPUMNormalAccesses;
  statistics::Scalar m_statPUMNormalAccessConflicts;
  statistics::Scalar m_statPUMNormalAccessDelayCycles;
  statistics::Formula m_statPUMNormalAccessAvgDelayCycles;

  statistics::Scalar m_statPUMIntraArrayShiftCycles;
  statistics::Scalar m_statPUMInterArrayShiftCycles;
  statistics::Scalar m_statPUMInterBankShiftCycles;
  statistics::Scalar m_statPUMIntraArrayShiftBits;
  statistics::Scalar m_statPUMInterArrayShiftBits;
  statistics::Scalar m_statPUMIntraArrayShiftBitHops;
  statistics::Scalar m_statPUMInterArrayShiftBitHops;
  statistics::Scalar m_statPUMInterBankShiftBits;
  statistics::Scalar m_statPUMInterBankShiftReuseBits;
};

} // namespace ruby
} // namespace gem5

#endif
