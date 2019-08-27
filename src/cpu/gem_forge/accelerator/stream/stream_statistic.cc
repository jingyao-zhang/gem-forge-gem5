#include "stream_statistic.hh"

void StreamStatistic::dump(std::ostream &os) const {
#define dumpScalar(stat) os << "  " #stat << ' ' << stat << '\n'
#define dumpAvg(name, dividend, divisor)                                       \
  {                                                                            \
    auto avg = (divisor > 0) ? dividend / divisor : 0;                         \
    os << "  " #name << ' ' << avg << '\n';                                    \
  }
  dumpScalar(numConfigured);
  dumpScalar(numAllocated);
  dumpScalar(numFetched);
  dumpScalar(numStepped);
  dumpScalar(numUsed);

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
