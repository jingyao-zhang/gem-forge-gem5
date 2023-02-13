#include "run_time_profiler.hh"

#include "base/output.hh"

namespace gem5 {

void RunTimeProfiler::profileLoadLatency(Addr pc, uint64_t latency) {
  auto &record =
      this->PCLoadLatencyMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(pc),
                   std::forward_as_tuple())
          .first->second;
  record.totalLatency += latency;
  record.executeTimes++;
}

void RunTimeProfiler::dump(const std::string &fn) const {
  auto outputStream = simout.findOrCreate(fn);
  auto &stream = *outputStream->stream();
  for (const auto &PCRecord : this->PCLoadLatencyMap) {
    const auto &Record = PCRecord.second;
    stream << PCRecord.first << ' '
           << static_cast<float>(Record.totalLatency) /
                  static_cast<float>(Record.executeTimes)
           << '\n';
  }
  simout.close(outputStream);
}} // namespace gem5

