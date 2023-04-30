#ifndef __CPU_TDG_ACCELERATOR_STREAM_FLOAT_TRACER_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_FLOAT_TRACER_HH__

#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"
#include "mem/ruby/common/MachineID.hh"
#include "proto/protoio.hh"

#include <memory>
#include <vector>

namespace gem5 {

/**
 * Trace floating stream event in LLC.
 */

class StreamFloatTracer {
public:
  StreamFloatTracer(const int _cpuId, const std::string &_name)
      : cpuId(_cpuId), name(_name) {}

  void traceEvent(
      uint64_t cycle, ruby::MachineID machineId,
      const ::LLVM::TDG::StreamFloatEvent::StreamFloatEventType &type) const;

  void dump() const {
    if (this->protoStream) {
      this->write();
      // Simply reset.
      this->protoStream = nullptr;
    }
  }

  void write() const;

  // This will clear all results, including previous trace.
  void reset() const;

private:
  void initialize() const;

  const int cpuId;
  const std::string name;
  static constexpr int DUMP_THRESHOLD = 1024;
  mutable std::unique_ptr<ProtoOutputStream> protoStream = nullptr;

  mutable std::vector<::LLVM::TDG::StreamFloatEvent> buffer;
  mutable int used = 0;

  void openProtobufStream() const;
};

} // namespace gem5

#endif