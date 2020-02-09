#ifndef __CPU_TDG_ACCELERATOR_STREAM_STATISTIC_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_STATISTIC_HH__

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
  size_t numAllocated = 0;
  size_t numFetched = 0;
  size_t numStepped = 0;
  size_t numUsed = 0;
  size_t numAliased = 0;
  size_t numFaulted = 0;

  // Float statistics.
  size_t numMLCAllocatedSlice = 0;
  size_t numLLCSentSlice = 0;
  size_t numLLCFaultSlice = 0;

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

  StreamStatistic() = default;
  void dump(std::ostream &os) const;
};

#endif