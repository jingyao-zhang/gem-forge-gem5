#ifndef __CPU_TDG_RUN_TIME_PROFILER_HH__
#define __CPU_TDG_RUN_TIME_PROFILER_HH__

#include "base/types.hh"

#include <string>
#include <unordered_map>

namespace gem5 {

class RunTimeProfiler {
public:
  RunTimeProfiler() = default;

  RunTimeProfiler(const RunTimeProfiler &other) = delete;
  RunTimeProfiler &operator=(const RunTimeProfiler &other) = delete;

  RunTimeProfiler(RunTimeProfiler &&other) = delete;
  RunTimeProfiler &operator=(RunTimeProfiler &&other) = delete;

  void profileLoadLatency(Addr pc, uint64_t latency);

  void dump(const std::string &fn) const;

private:
  struct LoadLatency {
    uint64_t totalLatency;
    uint64_t executeTimes;
    LoadLatency() : totalLatency(0), executeTimes(0) {}
  };

  std::unordered_map<Addr, LoadLatency> PCLoadLatencyMap;
};

} // namespace gem5

#endif