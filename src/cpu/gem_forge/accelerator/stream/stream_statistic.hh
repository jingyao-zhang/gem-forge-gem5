#ifndef __CPU_TDG_ACCELERATOR_STREAM_STATISTIC_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_STATISTIC_HH__

#include <array>
#include <cassert>
#include <map>
#include <ostream>

namespace gem5 {

/**
 * Separate the stream statistic from Stream to a separate structure.
 */

struct StreamStatistic {
public:
  const uint64_t staticStreamId;

  /**
   * A static map from StaticStreamId to Statistics.
   * Used to aggregate stats of same static stream across the system.
   */
  static std::map<uint64_t, StreamStatistic> staticStats;
  static StreamStatistic &getStaticStat(uint64_t staticStreamId);
  StreamStatistic &getStaticStat() {
    return StreamStatistic::getStaticStat(this->staticStreamId);
  }

  StreamStatistic(uint64_t _staticStreamId) : staticStreamId(_staticStreamId) {}

  struct SingleAvgSampler {
    size_t samples = 0;
    size_t value = 0;
    size_t curCycle = 0;
    void sample(size_t v) {
      this->samples++;
      this->value += v;
    }
    void sample(size_t curCycle, size_t v) {
      this->value += v;
      if (curCycle != this->curCycle) {
        this->samples++;
        this->curCycle = curCycle;
      }
    }
    void clear() {
      this->samples = 0;
      this->value = 0;
      this->curCycle = 0;
    }
  };

  /**
   * Per stream statistics.
   *
   * I use direct member initializer here for better readability.
   * Member initializer list in the constructor is too verbose.
   */
  size_t numConfigured = 0;
  size_t numMisConfigured = 0;
  size_t numFloated = 0;
  size_t numFloatMem = 0;
  size_t numFloatPUM = 0;
  size_t numFloatRewinded = 0;
  size_t numFloatCancelled = 0;
  size_t numPseudoFloated = 0;
  size_t numFineGrainedOffloaded = 0;
  size_t numAllocated = 0;
  size_t numWithdrawn = 0;
  size_t numFetched = 0;
  size_t numPrefetched = 0;
  size_t numNDCed = 0;
  size_t numStepped = 0;
  size_t numUsed = 0;
  size_t numAliased = 0;
  size_t numFlushed = 0;
  size_t numFaulted = 0;
  size_t numCycle = 0;

  size_t numSample = 0;
  size_t numInflyRequest = 0;
  size_t maxSize = 0;
  size_t allocSize = 0;
  size_t numDynStreams = 0;

  // Float statistics.
  size_t numMLCAllocatedSlice = 0;
  size_t numLLCIssueSlice = 0;
  size_t numLLCSentSlice = 0;
  size_t numLLCMulticastSlice = 0;
  size_t numLLCCanMulticastSlice = 0;
  size_t numLLCFaultSlice = 0;
  size_t numLLCPredYSlice = 0;
  size_t numLLCPredNSlice = 0;
  size_t numRemoteMulticastSlice = 0;

  // Strand statistics.
  size_t numStrands = 0;
  size_t numPrefetchStrands = 0;

  // Float statistics in Mem.
  size_t numMemIssueSlice = 0;
  size_t numRemoteReuseSlice = 0;
  size_t numRemoteConfig = 0;
  size_t numRemoteConfigNoCCycle = 0;
  size_t numRemoteConfigCycle = 0;
  size_t numRemoteMigrate = 0;
  size_t numRemoteMigrateCycle = 0;
  size_t numRemoteMigrateDelayCycle = 0;
  size_t numRemoteRunCycle = 0;

  // Latency experienced by the core.
  size_t numCoreEarlyElement = 0;
  size_t numCoreEarlyCycle = 0;
  size_t numCoreLateElement = 0;
  size_t numCoreLateCycle = 0;

  // Latency experienced at MLC.
  size_t numMLCEarlySlice = 0;
  size_t numMLCEarlyCycle = 0;
  size_t numMLCLateSlice = 0;
  size_t numMLCLateCycle = 0;

  // Latency experienced at LLC.
  size_t numLLCEarlyElement = 0;
  size_t numLLCEarlyCycle = 0;
  size_t numLLCLateElement = 0;
  size_t numLLCLateCycle = 0;
  SingleAvgSampler remoteIssueToValueReadyCycle;
  struct LLCElementSample {
    size_t firstCheckCycle = 0;
    size_t readyToIssueCycle = 0;
    size_t issueCycle = 0;
    size_t valueReadyCycle = 0;
  };
  void sampleLLCElement(const LLCElementSample &s);

  size_t numIssuedRequest = 0;
  size_t numIssuedReadExRequest = 0;
  size_t numIssuedPrefetchRequest = 0;
  size_t numCycleRequestLatency = 0;
  size_t numMissL0 = 0;
  size_t numMissL1 = 0;
  size_t numMissL2 = 0;

  // RemoteNestConfigStats
  size_t remoteNestConfigMaxRegions = 0;

  // Compute statistics.
  size_t numLLCComputation = 0;
  size_t numLLCComputationComputeLatency = 0;
  size_t numLLCComputationWaitLatency = 0;
  size_t numFloatAtomic = 0;
  size_t numFloatAtomicRecvCommitCycle = 0;
  size_t numFloatAtomicWaitForCommitCycle = 0;
  size_t numFloatAtomicWaitForLockCycle = 0;
  size_t numFloatAtomicWaitForUnlockCycle = 0;
  using SrcDestStatsT = std::map<std::pair<int, int>, size_t>;
  static void sampleSrcDest(SrcDestStatsT &stats, int src, int dest) {
    stats
        .emplace(std::piecewise_construct, std::forward_as_tuple(src, dest),
                 std::forward_as_tuple(0))
        .first->second++;
  }
  static void dumpSrcDest(const SrcDestStatsT &stats, std::ostream &os);
  SrcDestStatsT numLLCSendTo;
  // Traffic pair from Remote to Local
  SrcDestStatsT remoteToLocalMsg;

  SrcDestStatsT numRemoteNestConfig;
  void sampleLLCSendTo(int from, int to) {
    sampleSrcDest(this->numLLCSendTo, from, to);
    sampleSrcDest(getStaticStat(this->staticStreamId).numLLCSendTo, from, to);
  }
  void sampleRemoteNestConfig(int from, int to) {
    sampleSrcDest(this->numRemoteNestConfig, from, to);
    sampleSrcDest(getStaticStat(this->staticStreamId).numRemoteNestConfig, from,
                  to);
  }

  // Ideal stream data traffic.
  size_t idealDataTrafficFix = 0;
  size_t idealDataTrafficCached = 0;
  size_t idealDataTrafficFloat = 0;

  // LLCStreamEngine issue statistics.
  enum LLCStreamEngineIssueReason {
    Issued = 0,
    IndirectPriority,
    NextSliceNotAllocated,
    NextSliceOverTripCount,
    MulticastPolicy,
    IssueClearCycle,
    MaxInflyRequest,
    MaxEngineInflyRequest,
    MaxIssueWidth,
    PendingMigrate,
    AliasedIndirectUpdate,
    BaseValueNotReady,
    ValueNotReady,
    WaitingPUM,
    NumLLCStreamEngineIssueReason,
  };
  // Will be default initialized.
  std::array<size_t, NumLLCStreamEngineIssueReason> llcIssueReasons = {};

  static const char *llcSEIssueReasonToString(LLCStreamEngineIssueReason r);

  void sampleLLCStreamEngineIssueReason(LLCStreamEngineIssueReason reason) {
    this->llcIssueReasons.at(reason)++;
  }

  void dump(std::ostream &os) const;
  void clear();

  SingleAvgSampler remoteForwardNoCDelay;
  // Delay from ready to issue.
  SingleAvgSampler remoteReadyToIssueDelay;
  // Delay from issuing element to message creation.
  SingleAvgSampler remoteIssueToReqGenDelay;
  // Delay from message creation to arriving at indirect bank.
  SingleAvgSampler remoteIndReqNoCDelay;
  // Delay from message creation to StreamReqBuffer injection.
  SingleAvgSampler remoteIndReqStreamBufInjectDelay;
  // Delay from message creation to MessageBuffer injection.
  SingleAvgSampler remoteIndReqMsgBufInjectDelay;
  // Delay from message creation to NoC injection.
  SingleAvgSampler remoteIndReqNoCInjectDelay;
  // Delay from message creation to received at MLC for StreamAck.
  SingleAvgSampler remoteToLocalAckNoCDelay;
  SingleAvgSampler llcReqLat;
  SingleAvgSampler memReqLat;
  // Sample local allocated slices.
  SingleAvgSampler localAliveSlice;
  // Sample local alive slice but credit not sent yet.
  SingleAvgSampler localCreditNotSentSlice;
  // Sample local alive slice and credit sent
  SingleAvgSampler localCreditSentSlice;
  // Sample local alive slice in different core status.
  SingleAvgSampler localWaitAckSlice;
  SingleAvgSampler localWaitDataSlice;
  SingleAvgSampler localAckReadySlice;
  SingleAvgSampler localDoneSlice;
  // Sample remote alive elem.
  SingleAvgSampler remoteAliveElem;
  // Sample remote element in different state.
  SingleAvgSampler remoteInitializedElem;
  SingleAvgSampler remoteReadyToIssueElem;
  SingleAvgSampler remoteIssuedElem;
  SingleAvgSampler remotePredicatedOffElem;
  // Sampled remote infly request.
  SingleAvgSampler remoteInflyReq;
  // Sampled remote infly computation.
  SingleAvgSampler remoteInflyCmp;

  /**
   * Collect the cycles between PUM sync.
   * So far we assume we have at most 16 sync per compuation.
   */
  static constexpr int MAX_SYNCS = 16;
  std::array<SingleAvgSampler, MAX_SYNCS> pumCyclesBetweenSync;
  void samplePUMCyclesBetweenSync(size_t cycles, int syncIdx) {
    assert(syncIdx < MAX_SYNCS);
    this->pumCyclesBetweenSync.at(syncIdx).sample(cycles);
  }
};

} // namespace gem5

#endif