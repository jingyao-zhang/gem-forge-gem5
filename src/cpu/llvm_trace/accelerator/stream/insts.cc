#include "insts.hh"

#include "base/misc.hh"
#include "base/trace.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "stream_engine.hh"

#include "debug/StreamEngine.hh"

StreamInst::StreamInst(const LLVM::TDG::TDGInstruction &_TDG)
    : LLVMDynamicInst(_TDG, 1), finished(false) {}

void StreamInst::markFinished() {
  DPRINTF(StreamEngine, "Mark StreamInst completed.\n");
  this->finished = true;
}

StreamConfigInst::StreamConfigInst(const LLVM::TDG::TDGInstruction &_TDG)
    : StreamInst(_TDG) {
  if (!this->TDG.has_stream_config()) {
    panic("StreamConfigInst with missing protobuf field.");
  }
  DPRINTF(StreamEngine, "Parsed StreamConfigInst to configure stream %lu, %s\n",
          this->TDG.stream_config().stream_name().c_str(),
          this->TDG.stream_config().stream_id());
}

void StreamConfigInst::execute(LLVMTraceCPU *cpu) {
  cpu->getAcceleratorManager()->handle(this);
}

void StreamConfigInst::commit(LLVMTraceCPU *cpu) {
  DPRINTF(StreamEngine, "Commit stream configure %lu\n", this->getSeqNum());
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      auto streamId = dep.dependent_id();
      cpu->getAcceleratorManager()->getStreamEngine()->commitStreamUser(
          streamId, this);
    }
  }
  cpu->getAcceleratorManager()->getStreamEngine()->commitStreamConfigure(this);
}

uint64_t StreamConfigInst::getStreamId() const {
  return this->TDG.stream_config().stream_id();
}

StreamStepInst::StreamStepInst(const LLVM::TDG::TDGInstruction &_TDG)
    : StreamInst(_TDG) {
  if (!this->TDG.has_stream_step()) {
    panic("StreamStepInst with missing protobuf field.");
  }
  DPRINTF(StreamEngine, "Parsed StreamStepInst to step stream %lu.\n",
          this->TDG.stream_step().stream_id());
}

bool StreamStepInst::isDependenceReady(LLVMTraceCPU *cpu) const {
  if (!this->LLVMDynamicInst::isDependenceReady(cpu)) {
    return false;
  }
  // For step instruction we also have to check if the stream can be stepped.
  return cpu->getAcceleratorManager()->getStreamEngine()->canStreamStep(
      this->TDG.stream_step().stream_id());
}

void StreamStepInst::execute(LLVMTraceCPU *cpu) {
  cpu->getAcceleratorManager()->handle(this);
}

void StreamStepInst::commit(LLVMTraceCPU *cpu) {
  DPRINTF(StreamEngine, "Commit stream step %lu\n", this->getSeqNum());
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      auto streamId = dep.dependent_id();
      cpu->getAcceleratorManager()->getStreamEngine()->commitStreamUser(
          streamId, this);
    }
  }
  cpu->getAcceleratorManager()->getStreamEngine()->commitStreamStep(this);
}

uint64_t StreamStepInst::getStreamId() const {
  return this->TDG.stream_step().stream_id();
}

StreamStoreInst::StreamStoreInst(const LLVM::TDG::TDGInstruction &_TDG)
    : StreamInst(_TDG) {
  if (!this->TDG.has_stream_store()) {
    panic("StreamStoreInst with missing protobuf field.");
  }
  DPRINTF(StreamEngine, "Parsed StreamStoreInst to store stream %lu.\n",
          this->TDG.stream_store().stream_id());
}

void StreamStoreInst::execute(LLVMTraceCPU *cpu) {
  // Notify the stream engine.
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      auto streamId = dep.dependent_id();
      cpu->getAcceleratorManager()->getStreamEngine()->commitStreamUser(
          streamId, this);
    }
  }
  cpu->getAcceleratorManager()->handle(this);
}

void StreamStoreInst::commit(LLVMTraceCPU *cpu) {
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      auto streamId = dep.dependent_id();
      cpu->getAcceleratorManager()->getStreamEngine()->commitStreamUser(
          streamId, this);
    }
  }
  cpu->getAcceleratorManager()->getStreamEngine()->commitStreamStore(this);
}

uint64_t StreamStoreInst::getStreamId() const {
  return this->TDG.stream_store().stream_id();
}

StreamEndInst::StreamEndInst(const LLVM::TDG::TDGInstruction &_TDG)
    : StreamInst(_TDG) {
  if (!this->TDG.has_stream_end()) {
    panic("StreamEndInst with missing protobuf field.");
  }
  DPRINTF(StreamEngine, "Parsed StreamEndInst to stream %lu.\n",
          this->TDG.stream_end().stream_id());
}

void StreamEndInst::execute(LLVMTraceCPU *cpu) {
  cpu->getAcceleratorManager()->handle(this);
}

void StreamEndInst::commit(LLVMTraceCPU *cpu) {
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      auto streamId = dep.dependent_id();
      cpu->getAcceleratorManager()->getStreamEngine()->commitStreamUser(
          streamId, this);
    }
  }
  cpu->getAcceleratorManager()->getStreamEngine()->commitStreamEnd(this);
}

uint64_t StreamEndInst::getStreamId() const {
  return this->TDG.stream_end().stream_id();
}

LLVMDynamicInst *parseStreamInst(LLVM::TDG::TDGInstruction &TDGInst) {
  switch (TDGInst.extra_case()) {
    case LLVM::TDG::TDGInstruction::ExtraCase::kStreamConfig: {
      return new StreamConfigInst(TDGInst);
    }
    case LLVM::TDG::TDGInstruction::ExtraCase::kStreamStep: {
      return new StreamStepInst(TDGInst);
    }
    case LLVM::TDG::TDGInstruction::ExtraCase::kStreamStore: {
      return new StreamStoreInst(TDGInst);
    }
    case LLVM::TDG::TDGInstruction::ExtraCase::kStreamEnd: {
      return new StreamEndInst(TDGInst);
    }
    default: { break; }
  }

  return nullptr;
}