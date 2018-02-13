#include "cpu/llvm_trace/llvm_commit_stage.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMCommitStage::LLVMCommitStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu)
    : cpu(_cpu), fromIEWDelay(params->iewToCommitDelay) {}

void LLVMCommitStage::setFromIEW(TimeBuffer<IEWStruct>* fromIEWBuffer) {
  this->fromIEW = fromIEWBuffer->getWire(-this->fromIEWDelay);
}

void LLVMCommitStage::setSignal(TimeBuffer<LLVMStageSignal>* signalBuffer,
                                int pos) {
  this->signal = signalBuffer->getWire(pos);
}

void LLVMCommitStage::regStats() {
  // No stats for now.
}

void LLVMCommitStage::tick() {
  // Read fromIEW.
  // NOTE: For now commit stage will never raise stall signal.
  for (auto iter = this->fromIEW->begin(), end = this->fromIEW->end();
       iter != end; ++iter) {
    auto instId = *iter;
    panic_if(cpu->inflyInsts.find(instId) == cpu->inflyInsts.end(),
             "Inst %u should be in inflyInsts to be commited\n", instId);
    panic_if(cpu->inflyInsts.at(instId) != InstStatus::FINISHED,
             "Inst %u should be finished to be commited, not %d\n", instId,
             cpu->inflyInsts.at(instId));

    DPRINTF(LLVMTraceCPU, "Inst %u committed, remaining infly inst #%u\n",
            instId, cpu->inflyInsts.size());
    cpu->inflyInsts.erase(instId);
  }
}
