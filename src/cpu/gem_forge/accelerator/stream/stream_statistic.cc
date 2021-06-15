#include "stream_statistic.hh"

#include <cassert>
#include <iomanip>

void StreamStatistic::dump(std::ostream &os) const {
#define dumpScalar(stat)                                                       \
  os << std::setw(40) << "  " #stat << ' ' << stat << '\n'
#define dumpScalarIfNonZero(stat)                                              \
  if (stat != 0) {                                                             \
    dumpScalar(stat);                                                          \
  }
#define dumpNamedScalar(name, stat)                                            \
  os << std::setw(40) << (name) << ' ' << (stat) << '\n'
#define dumpAvg(name, dividend, divisor)                                       \
  {                                                                            \
    auto avg = (divisor > 0) ? dividend / divisor : 0;                         \
    os << std::setw(40) << "  " #name << ' ' << avg << '\n';                   \
  }
#define dumpSingleAvgSample(name)                                              \
  {                                                                            \
    if (name.samples > 0) {                                                    \
      dumpScalar(name.samples);                                                \
      dumpAvg(name.avg, name.value, name.samples);                             \
    }                                                                          \
  }
  dumpScalar(numConfigured);
  dumpScalar(numMisConfigured);
  dumpScalar(numFloated);
  dumpScalar(numFloatRewinded);
  dumpScalar(numFloatCancelled);
  dumpScalar(numPseudoFloated);
  dumpScalarIfNonZero(numFineGrainedOffloaded);
  dumpScalar(numAllocated);
  dumpScalar(numWithdrawn);
  dumpScalar(numFetched);
  dumpScalar(numPrefetched);
  dumpScalarIfNonZero(numNDCed);
  dumpScalar(numStepped);
  dumpScalar(numUsed);
  dumpScalar(numAliased);
  dumpScalar(numFlushed);
  dumpScalar(numFaulted);
  dumpScalar(numCycle);
  dumpAvg(avgTurnAroundCycle, numCycle, numStepped);

  dumpScalar(numSample);
  dumpAvg(avgInflyRequest, numInflyRequest, numSample);
  dumpAvg(avgMaxSize, maxSize, numSample);
  dumpAvg(avgAllocSize, allocSize, numSample);
  dumpAvg(avgNumDynStreams, numDynStreams, numSample);

  dumpScalar(numMLCAllocatedSlice);
  dumpScalar(numLLCIssueSlice);
  dumpScalar(numLLCSentSlice);
  dumpScalar(numLLCMulticastSlice);
  dumpScalar(numLLCCanMulticastSlice);
  dumpScalar(numLLCFaultSlice);
  dumpScalar(numLLCPredYSlice);
  dumpScalar(numLLCPredNSlice);
  dumpScalar(numLLCMigrate);
  dumpScalar(numLLCMigrateCycle);
  dumpAvg(avgMigrateCycle, numLLCMigrateCycle, numLLCMigrate);

  dumpScalar(numLLCAliveElementSamples);
  if (numLLCAliveElementSamples > 0) {
    dumpAvg(avgLLCAliveElements, numLLCAliveElements,
            numLLCAliveElementSamples);
  }

  dumpAvg(avgLength, numStepped, numConfigured);
  dumpAvg(avgUsed, numUsed, numConfigured);

  dumpScalar(numCoreEarlyElement);
  dumpScalar(numCoreEarlyCycle);
  dumpAvg(avgCoreEarlyCycle, numCoreEarlyCycle, numCoreEarlyElement);

  dumpScalar(numCoreLateElement);
  dumpScalar(numCoreLateCycle);
  dumpAvg(avgCoreLateCycle, numCoreLateCycle, numCoreLateElement);

  if (numMLCEarlySlice > 0 || numMLCLateSlice > 0) {
    dumpScalar(numMLCEarlySlice);
    dumpAvg(avgMLCEarlyCycle, numMLCEarlyCycle, numMLCEarlySlice);
    dumpScalar(numMLCLateSlice);
    dumpAvg(avgMLCLateCycle, numMLCLateCycle, numMLCLateSlice);
  }
  if (numLLCEarlyElement > 0 || numLLCLateElement > 0) {
    dumpScalar(numLLCEarlyElement);
    dumpAvg(avgLLCEarlyCycle, numLLCEarlyCycle, numLLCEarlyElement);
    dumpScalar(numLLCLateElement);
    dumpAvg(avgLLCLateCycle, numLLCLateCycle, numLLCLateElement);
  }
  dumpSingleAvgSample(llcForwardLat);

  dumpScalar(numIssuedRequest);
  dumpScalar(numIssuedReadExRequest);
  dumpScalar(numIssuedPrefetchRequest);
  dumpScalar(numCycleRequestLatency);
  dumpAvg(avgRequestLatency, numCycleRequestLatency, numIssuedRequest);

  if (this->idealDataTrafficFix > 0) {
    dumpScalar(idealDataTrafficFix);
    dumpScalar(idealDataTrafficCached);
    dumpScalar(idealDataTrafficFloat);
  }

  dumpScalar(numLLCComputation);
  if (numLLCComputation > 0) {
    dumpAvg(avgLLCComputeLatency, numLLCComputationComputeLatency,
            numLLCComputation);
    dumpAvg(avgLLCWaitComputeLatency, numLLCComputationWaitLatency,
            numLLCComputation);
  }

  dumpScalar(numFloatAtomic);
  if (numFloatAtomic > 0) {
    dumpAvg(avgFloatAtomicRecvCommitCycle, numFloatAtomicRecvCommitCycle,
            numFloatAtomic);
    dumpAvg(avgFloatAtomicWaitForLockCycle, numFloatAtomicWaitForLockCycle,
            numFloatAtomic);
    dumpAvg(avgFloatAtomicWaitForCommitCycle, numFloatAtomicWaitForCommitCycle,
            numFloatAtomic);
    dumpAvg(avgFloatAtomicWaitForUnlockCycle, numFloatAtomicWaitForUnlockCycle,
            numFloatAtomic);
  }

  if (numLLCInflyComputationSample > 0) {
    dumpAvg(avgLLCInflyComputation, numLLCInflyComputation,
            numLLCInflyComputationSample);
  }

  if (!this->numLLCSendTo.empty()) {
    size_t total = 0;
    for (const auto &entry : this->numLLCSendTo) {
      total += entry.second;
    }
    for (const auto &entry : this->numLLCSendTo) {
      auto from = entry.first.first;
      auto to = entry.first.second;
      auto ratio = static_cast<float>(entry.second) / static_cast<float>(total);
      os << std::setw(5) << from << " -> " << std::setw(5) << to << " "
         << std::setw(10) << entry.second << " " << ratio << "\n";
    }
  }

  dumpScalar(numMissL0);
  dumpScalar(numMissL1);
  dumpScalar(numMissL2);

  for (auto idx = 0; idx < this->llcIssueReasons.size(); ++idx) {
    if (this->llcIssueReasons.at(idx) > 0) {
      dumpNamedScalar(llcSEIssueReasonToString(
                          static_cast<LLCStreamEngineIssueReason>(idx)),
                      this->llcIssueReasons.at(idx));
    }
  }

#undef dumpScalar
#undef dumpAvg
}

const char *
StreamStatistic::llcSEIssueReasonToString(LLCStreamEngineIssueReason reason) {
#define Case(x)                                                                \
  case x:                                                                      \
    return #x
  switch (reason) {
    Case(Issued);
    Case(IndirectPriority);
    Case(NextSliceNotAllocated);
    Case(MulticastPolicy);
    Case(IssueClearCycle);
    Case(MaxInflyRequest);
    Case(PendingMigrate);
    Case(AliasedIndirectUpdate);
    Case(BaseValueNotReady);
    Case(ValueNotReady);
    Case(NumLLCStreamEngineIssueReason);
#undef Case
  default:
    assert(false && "Invalid LLCStreamEngineIssueReason.");
  }
}

void StreamStatistic::clear() {
  this->numConfigured = 0;
  this->numMisConfigured = 0;
  this->numFloated = 0;
  this->numFloatRewinded = 0;
  this->numFloatCancelled = 0;
  this->numPseudoFloated = 0;
  this->numFineGrainedOffloaded = 0;
  this->numAllocated = 0;
  this->numFetched = 0;
  this->numNDCed = 0;
  this->numStepped = 0;
  this->numUsed = 0;
  this->numAliased = 0;
  this->numFlushed = 0;
  this->numFaulted = 0;
  this->numCycle = 0;
  this->numSample = 0;
  this->numInflyRequest = 0;
  this->maxSize = 0;
  this->allocSize = 0;
  this->numDynStreams = 0;
  this->numMLCAllocatedSlice = 0;
  this->numLLCIssueSlice = 0;
  this->numLLCSentSlice = 0;
  this->numLLCMulticastSlice = 0;
  this->numLLCCanMulticastSlice = 0;
  this->numLLCFaultSlice = 0;
  this->numLLCPredYSlice = 0;
  this->numLLCPredNSlice = 0;
  this->numLLCMigrate = 0;
  this->numLLCMigrateCycle = 0;
  this->numCoreEarlyElement = 0;
  this->numCoreEarlyCycle = 0;
  this->numCoreLateElement = 0;
  this->numCoreLateCycle = 0;
  this->numMLCEarlySlice = 0;
  this->numMLCEarlyCycle = 0;
  this->numMLCLateSlice = 0;
  this->numMLCLateCycle = 0;
  this->numLLCEarlyElement = 0;
  this->numLLCEarlyCycle = 0;
  this->numLLCLateElement = 0;
  this->numLLCLateCycle = 0;
  this->numIssuedRequest = 0;
  this->numIssuedReadExRequest = 0;
  this->numIssuedPrefetchRequest = 0;
  this->numCycleRequestLatency = 0;
  this->numMissL0 = 0;
  this->numMissL1 = 0;
  this->numMissL2 = 0;

  this->numLLCComputation = 0;
  this->numLLCComputationComputeLatency = 0;
  this->numLLCComputationWaitLatency = 0;
  this->numFloatAtomic = 0;
  this->numFloatAtomicRecvCommitCycle = 0;
  this->numFloatAtomicWaitForCommitCycle = 0;
  this->numFloatAtomicWaitForLockCycle = 0;
  this->numFloatAtomicWaitForUnlockCycle = 0;
  this->numLLCSendTo.clear();

  this->numLLCAliveElements = 0;
  this->numLLCAliveElementSamples = 0;
  this->numLLCInflyComputation = 0;
  this->numLLCInflyComputationSample = 0;

  this->idealDataTrafficFix = 0;
  this->idealDataTrafficCached = 0;
  this->idealDataTrafficFloat = 0;

  this->llcForwardLat.clear();

  for (auto &reasons : this->llcIssueReasons) {
    reasons = 0;
  }
}