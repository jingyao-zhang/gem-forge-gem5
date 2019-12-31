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
  dumpScalar(numAllocated);
  dumpScalar(numFetched);
  dumpScalar(numStepped);
  dumpScalar(numUsed);
  dumpScalar(numAliased);
  dumpScalar(numFaulted);

  dumpScalar(numMLCAllocatedSlice);
  dumpScalar(numLLCSentSlice);
  dumpScalar(numLLCFaultSlice);

  dumpAvg(avgLength, numStepped, numConfigured);
  dumpAvg(avgUsed, numUsed, numConfigured);

  dumpScalar(numCoreEarlyElement);
  dumpScalar(numCycleCoreEarlyElement);
  dumpAvg(avgCoreEarlyCycle, numCycleCoreEarlyElement, numCoreEarlyElement);

  dumpScalar(numCoreLateElement);
  dumpScalar(numCycleCoreLateElement);
  dumpAvg(avgCoreLateCycle, numCycleCoreLateElement, numCoreLateElement);

  dumpScalar(numIssuedRequest);
  dumpScalar(numCycleRequestLatency);
  dumpAvg(avgRequestLatency, numCycleRequestLatency, numIssuedRequest);

  dumpScalar(numMissL0);
  dumpScalar(numMissL1);
  dumpScalar(numMissL2);
#undef dumpScalar
#undef dumpAvg
}
