#include "stream_float_tracer.hh"

#include "base/output.hh"

#include <sstream>

namespace gem5 {

void StreamFloatTracer::traceEvent(
    uint64_t cycle, ruby::MachineID machineId,
    const ::LLVM::TDG::StreamFloatEvent::StreamFloatEventType &type) const {
  if (this->buffer.size() == 0) {
    // Initialize.
    this->initialize();
  }
  if (this->used == StreamFloatTracer::DUMP_THRESHOLD) {
    // Time to write.
    this->write();
  }
  auto &entry = this->buffer.at(this->used);
  entry.Clear();
  entry.set_cycle(cycle);
  entry.set_llc_bank(machineId.getNum());
  entry.set_type(type);
  switch (machineId.getType()) {
  default:
    panic("Unsupported FloatTracer on %s.", machineId);
    break;
  case ruby::MachineType_L2Cache:
    entry.set_se(LLVM::TDG::StreamFloatEvent::StreamEngineType::
                     StreamFloatEvent_StreamEngineType_LLC);
    break;
  case ruby::MachineType_Directory:
    entry.set_se(LLVM::TDG::StreamFloatEvent::StreamEngineType::
                     StreamFloatEvent_StreamEngineType_MEM);
    break;
  }
  this->used++;
}

void StreamFloatTracer::openProtobufStream() const {
  // Try to create the stream_float_trace folder.
  auto directory = simout.findOrCreateSubdirectory("stream_float_trace");
  std::stringstream ss;
  ss << this->cpuId << '-' << this->name << ".data";
  auto fileName = directory->resolve(ss.str());
  this->protoStream = std::make_unique<ProtoOutputStream>(fileName);
}

void StreamFloatTracer::initialize() const {
  this->buffer.resize(StreamFloatTracer::DUMP_THRESHOLD);
  this->used = 0;
  // Try to create the stream_float_trace folder.
  this->openProtobufStream();
}

void StreamFloatTracer::write() const {
  for (int i = 0; i < this->used; ++i) {
    this->protoStream->write(this->buffer.at(i));
  }
  this->used = 0;
}

void StreamFloatTracer::reset() const {
  if (this->buffer.empty()) {
    // Not initialized yet.
    return;
  }
  this->used = 0;
  // Reopen the protobuf stream will clear previous content.
  this->protoStream = nullptr;
  this->openProtobufStream();
}
} // namespace gem5
