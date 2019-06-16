#include "insts.hh"

// #include "base/misc.hh""
#include "base/trace.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
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
  DPRINTF(StreamEngine, "Parsed StreamConfigInst to configure loop %s\n",
          this->TDG.stream_config().loop().c_str());
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
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  SE->executeStreamConfigure(this);
  this->markFinished();
}

void StreamConfigInst::commit(LLVMTraceCPU *cpu) {
  DPRINTF(StreamEngine, "Commit stream configure %lu\n", this->getSeqNum());
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  SE->commitStreamConfigure(this);
}

uint64_t StreamConfigInst::getStreamId() const {
  panic("no more stream id for config.");
  return 0;
}

void StreamConfigInst::dumpBasic() const {
  inform("Inst seq %lu, id %lu, op %s, loop %s.\n", this->seqNum, this->getId(),
         this->getInstName().c_str(), this->TDG.stream_config().loop());
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

void StreamStepInst::dumpBasic() const {
  inform("Inst seq %lu, id %lu, op %s, stream %lu.\n", this->seqNum,
         this->getId(), this->getInstName().c_str(),
         this->TDG.stream_step().stream_id());
}

StreamStoreInst::StreamStoreInst(const LLVM::TDG::TDGInstruction &_TDG)
    : StreamInst(_TDG) {
  if (!this->TDG.has_stream_store()) {
    panic("StreamStoreInst with missing protobuf field.");
  }
  DPRINTF(StreamEngine, "Parsed StreamStoreInst to store stream %lu.\n",
          this->TDG.stream_store().stream_id());
}

bool StreamStoreInst::canDispatch(LLVMTraceCPU *cpu) const {
  // StreamStore may also be a stream user.
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  if (this->hasStreamUse()) {
    if (!SE->canStreamUserDispatch(this)) {
      return false;
    }
  }
  return SE->canStreamStoreDispatch(this);
}

void StreamStoreInst::dispatch(LLVMTraceCPU *cpu) {
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  if (this->hasStreamUse()) {
    SE->dispatchStreamUser(this);
  }
  SE->dispatchStreamStore(this);
}

void StreamStoreInst::execute(LLVMTraceCPU *cpu) {
  // Notify the stream engine.
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  if (this->hasStreamUse()) {
    SE->executeStreamUser(this);
  }
  SE->executeStreamStore(this);
  this->markFinished();
}

void StreamStoreInst::commit(LLVMTraceCPU *cpu) {
  auto SE = cpu->getAcceleratorManager()->getStreamEngine();
  if (this->hasStreamUse()) {
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
  DPRINTF(StreamEngine, "Parsed StreamEndInst to loop %s.\n",
          this->TDG.stream_end().loop().c_str());
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
  panic("no more stream id for end.");
  return 0;
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