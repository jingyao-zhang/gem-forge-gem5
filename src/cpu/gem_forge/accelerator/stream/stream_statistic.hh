#ifndef __CPU_TDG_ACCELERATOR_STREAM_STATISTIC_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_STATISTIC_HH__

#include <array>
#include <ostream>

/**
 * Separate the stream statistic from Stream to a separate structure.
 */

struct StreamStatistic {
public:
  /**
   * Per stream statistics.
   *
   * I use direct member initializer here for better readability.
   * Member initializer list in the constructor is too verbose.
   */
  size_t numConfigured = 0;
  size_t numMisConfigured = 0;
  size_t numFloated = 0;
  size_t numFloatRewinded = 0;
  size_t numFloatCancelled = 0;
  size_t numPseudoFloated = 0;
  size_t numAllocated = 0;
  size_t numFetched = 0;
  size_t numStepped = 0;
  size_t numUsed = 0;
  size_t numAliased = 0;
  size_t numFaulted = 0;
  size_t numCycle = 0;

  size_t numSample = 0;
  size_t numInflyRequest = 0;
  size_t maxSize = 0;
  void sampleStats(int numInflyRequest, int maxSize) {
    this->numSample++;
    this->numInflyRequest += numInflyRequest;
    this->maxSize += maxSize;
  }

  // Float statistics.
  size_t numMLCAllocatedSlice = 0;
  size_t numLLCIssueSlice = 0;
  size_t numLLCSentSlice = 0;
  size_t numLLCMulticastSlice = 0;
  size_t numLLCCanMulticastSlice = 0;
  size_t numLLCFaultSlice = 0;
  size_t numLLCPredYSlice = 0;
  size_t numLLCPredNSlice = 0;
  size_t numLLCMigrate = 0;
  size_t numLLCMigrateCycle = 0;

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

  size_t numIssuedRequest = 0;
  size_t numCycleRequestLatency = 0;
  size_t numMissL0 = 0;
  size_t numMissL1 = 0;
  size_t numMissL2 = 0;

  // LLCStreamEngine issue statistics.
  enum LLCStreamEngineIssueReason {
    Issued = 0,
    IndirectPriority,
    NextSliceNotAllocated,
    MulticastPolicy,
    IssueClearCycle,
    MaxInflyRequest,
    PendingMigrate,
    NumLLCStreamEngineIssueReason,
  };
  // Will be default initialized.
  std::array<size_t, NumLLCStreamEngineIssueReason> llcIssueReasons = {};

  static const char *llcSEIssueReasonToString(LLCStreamEngineIssueReason r);

  void sampleLLCStreamEngineIssueReason(LLCStreamEngineIssueReason reason) {
    this->llcIssueReasons.at(reason)++;
  }

  StreamStatistic() = default;
  void dump(std::ostream &os) const;
  void clear();
};

#endif