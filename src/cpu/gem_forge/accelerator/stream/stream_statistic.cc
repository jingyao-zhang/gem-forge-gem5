#include "stream_statistic.hh"

#include <iomanip>

void StreamStatistic::dump(std::ostream &os) const {
#define dumpScalar(stat)                                                       \
  os << std::setw(40) << "  " #stat << ' ' << stat << '\n'
#define dumpAvg(name, dividend, divisor)                                       \
  {                                                                            \
    auto avg = (divisor > 0) ? dividend / divisor : 0;                         \
    os << std::setw(40) << "  " #name << ' ' << avg << '\n';                   \
  }
  dumpScalar(numConfigured);
  dumpScalar(numMisConfigured);
  dumpScalar(numFloated);
  dumpScalar(numFloatCancelled);
  dumpScalar(numPseudoFloated);
  dumpScalar(numAllocated);
  dumpScalar(numFetched);
  dumpScalar(numStepped);
  dumpScalar(numUsed);
  dumpScalar(numAliased);
  dumpScalar(numFaulted);
  dumpScalar(numCycle);
  dumpAvg(avgTurnAroundCycle, numCycle, numStepped);

  dumpScalar(numInflyRequest);
  dumpScalar(numInflyRequestSample);
  dumpAvg(avgInflyRequest, numInflyRequest, numInflyRequestSample);

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

  dumpAvg(avgLength, numStepped, numConfigured);
  dumpAvg(avgUsed, numUsed, numConfigured);

  dumpScalar(numCoreEarlyElement);
  dumpScalar(numCoreEarlyCycle);
  dumpAvg(avgCoreEarlyCycle, numCoreEarlyCycle, numCoreEarlyElement);

  dumpScalar(numCoreLateElement);
  dumpScalar(numCoreLateCycle);
  dumpAvg(avgCoreLateCycle, numCoreLateCycle, numCoreLateElement);

  dumpScalar(numMLCEarlySlice);
  dumpScalar(numMLCEarlyCycle);
  dumpAvg(avgMLCEarlyCycle, numMLCEarlyCycle, numMLCEarlySlice);

  dumpScalar(numMLCLateSlice);
  dumpScalar(numMLCLateCycle);
  dumpAvg(avgMLCLateCycle, numMLCLateCycle, numMLCLateSlice);

  dumpScalar(numIssuedRequest);
  dumpScalar(numCycleRequestLatency);
  dumpAvg(avgRequestLatency, numCycleRequestLatency, numIssuedRequest);

  dumpScalar(numMissL0);
  dumpScalar(numMissL1);
  dumpScalar(numMissL2);

  for (const auto &llcIssueReason : this->llcIssueReasons) {
    dumpScalar(llcIssueReason);
  }

#undef dumpScalar
#undef dumpAvg
}
