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

bool StreamConfigInst::canDispatch(LLVMTraceCPU *cpu) const {
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  return SE->canStreamConfig(this);
}

void StreamConfigInst::dispatch(LLVMTraceCPU *cpu) {
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  SE->dispatchStreamConfigure(this);
}

void StreamConfigInst::execute(LLVMTraceCPU *cpu) {
  // Automatically finished.
  this->markFinished();
}

void StreamConfigInst::commit(LLVMTraceCPU *cpu) {
  DPRINTF(StreamEngine, "Commit stream configure %lu\n", this->getSeqNum());
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  SE->commitStreamConfigure(this);
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

bool StreamStepInst::canDispatch(LLVMTraceCPU *cpu) const {
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  return SE->canStreamStep(this);
}

void StreamStepInst::dispatch(LLVMTraceCPU *cpu) {
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  SE->dispatchStreamStep(this);
}

void StreamStepInst::execute(LLVMTraceCPU *cpu) { this->markFinished(); }

void StreamStepInst::commit(LLVMTraceCPU *cpu) {
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
  auto hasStreamUse = false;
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      hasStreamUse = true;
      break;
    }
  }
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  if (hasStreamUse) {
    SE->executeStreamUser(this);
  }
  SE->executeStreamStore(this);
  this->markFinished();
}

void StreamStoreInst::commit(LLVMTraceCPU *cpu) {
  auto hasStreamUse = false;
  for (const auto &dep : this->TDG.deps()) {
    if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      hasStreamUse = true;
      break;
    }
  }
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  if (hasStreamUse) {
    SE->commitStreamUser(this);
  }
  SE->commitStreamStore(this);
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

void StreamEndInst::dispatch(LLVMTraceCPU *cpu) {
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  SE->dispatchStreamEnd(this);
}

void StreamEndInst::execute(LLVMTraceCPU *cpu) { this->markFinished(); }

void StreamEndInst::commit(LLVMTraceCPU *cpu) {
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  SE->commitStreamEnd(this);
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