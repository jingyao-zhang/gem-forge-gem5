#include "insts.hh"

#include "base/misc.hh"
#include "base/trace.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/StreamEngine.hh"

StreamConfigInst::StreamConfigInst(const LLVM::TDG::TDGInstruction &_TDG)
    : LLVMDynamicInst(_TDG, 1), finished(false) {
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

void StreamConfigInst::markFinished() {
  DPRINTF(StreamEngine, "Mark StreamConfigInst completed.\n");
  this->finished = true;
}

StreamStepInst::StreamStepInst(const LLVM::TDG::TDGInstruction &_TDG)
    : LLVMDynamicInst(_TDG, 1), finished(false) {
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
  return cpu->getAcceleratorManager()->canStreamStep(
      this->TDG.stream_step().stream_id());
}

void StreamStepInst::execute(LLVMTraceCPU *cpu) {
  cpu->getAcceleratorManager()->handle(this);
}

void StreamStepInst::commit(LLVMTraceCPU *cpu) {
  DPRINTF(StreamEngine, "Commit stream step %lu\n", this->getSeqNum());
  cpu->getAcceleratorManager()->commitStreamStep(
      this->TDG.stream_step().stream_id(), this->getSeqNum());
}

void StreamStepInst::markFinished() {
  DPRINTF(StreamEngine, "Mark StreamStepInst completed.\n");
  this->finished = true;
}

StreamStoreInst::StreamStoreInst(const LLVM::TDG::TDGInstruction &_TDG)
    : LLVMDynamicInst(_TDG, 1), finished(false) {
  if (!this->TDG.has_stream_store()) {
    panic("StreamStoreInst with missing protobuf field.");
  }
  DPRINTF(StreamEngine, "Parsed StreamStoreInst to store stream %lu.\n",
          this->TDG.stream_store().stream_id());
}

void StreamStoreInst::execute(LLVMTraceCPU *cpu) {
  // Notify the stream engine.
  for (const auto &streamId : this->TDG.used_stream_ids()) {
    cpu->getAcceleratorManager()->useStream(streamId, this->seqNum);
  }
  cpu->getAcceleratorManager()->handle(this);
}

void StreamStoreInst::commit(LLVMTraceCPU *cpu) {
  cpu->getAcceleratorManager()->commitStreamStore(
      this->TDG.stream_store().stream_id(), this->getSeqNum());
}

void StreamStoreInst::markFinished() {
  DPRINTF(StreamEngine, "Mark StreamStoreInst completed.\n");
  this->finished = true;
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
  default: { break; }
  }

  return nullptr;
}