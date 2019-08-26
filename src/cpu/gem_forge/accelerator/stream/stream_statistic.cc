#include "stream_statistic.hh"

void StreamStatistic::dump(std::ostream &os) const {
#define dumpScalar(stat) os << "  " #stat << ' ' << stat << '\n'
  dumpScalar(numConfigured);
  dumpScalar(numAllocated);
  dumpScalar(numFetched);
  dumpScalar(numStepped);
  dumpScalar(numUsed);
  dumpScalar(numIssuedRequest);
  dumpScalar(numCycleRequestLatency);

  auto avgRequestLatency =
      (this->numIssuedRequest > 0)
          ? this->numCycleRequestLatency / this->numIssuedRequest
          : 0;
  dumpScalar(avgRequestLatency);

  dumpScalar(numMissL0);
  dumpScalar(numMissL1);
  dumpScalar(numMissL2);
#undef dumpScalar
}
