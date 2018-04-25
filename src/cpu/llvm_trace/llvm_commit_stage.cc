#include "cpu/llvm_trace/llvm_commit_stage.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMCommitStage::LLVMCommitStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu)
    : cpu(_cpu),
      commitWidth(params->commitWidth),
      commitQueueSize(params->commitQueueSize),
      fromIEWDelay(params->iewToCommitDelay) {}

void LLVMCommitStage::setFromIEW(TimeBuffer<IEWStruct>* fromIEWBuffer) {
  this->fromIEW = fromIEWBuffer->getWire(-this->fromIEWDelay);
}

void LLVMCommitStage::setSignal(TimeBuffer<LLVMStageSignal>* signalBuffer,
                                int pos) {
  this->signal = signalBuffer->getWire(pos);
}

void LLVMCommitStage::regStats() {
  // No stats for now.
  this->instsCommitted.init(cpu->numThreads)
      .name(cpu->name() + ".commit.committedInsts")
      .desc("Number of instructions committed")
      .flags(Stats::total);
  this->opsCommitted.init(cpu->numThreads)
      .name(cpu->name() + ".commit.committedOps")
      .desc("Number of ops (including micro ops) committed")
      .flags(Stats::total);
  this->blockedCycles.name(cpu->name() + ".commit.blockedCycles")
      .desc("Number of cycles blocked")
      .prereq(this->blockedCycles);
}

void LLVMCommitStage::tick() {
  // Read fromIEW.
  // NOTE: For now commit stage will never raise stall signal.
  for (auto iter = this->fromIEW->begin(), end = this->fromIEW->end();
       iter != end; ++iter) {
    auto instId = *iter;

    this->commitQueue.push_back(instId);
  }

  unsigned committedInsts = 0;
  while (!this->commitQueue.empty() && committedInsts < this->commitWidth) {
    auto instId = this->commitQueue.front();

    if (committedInsts + cpu->dynamicInsts[instId]->getQueueWeight() >
        this->commitWidth) {
      break;
    }

    committedInsts += cpu->dynamicInsts[instId]->getQueueWeight();
    this->commitQueue.pop_front();

    panic_if(cpu->inflyInsts.find(instId) == cpu->inflyInsts.end(),
             "Inst %u should be in inflyInsts to be commited\n", instId);
    panic_if(cpu->inflyInsts.at(instId) != InstStatus::FINISHED,
             "Inst %u should be finished to be commited, not %d\n", instId,
             cpu->inflyInsts.at(instId));

    DPRINTF(LLVMTraceCPU, "Inst %u committed, remaining infly inst #%u\n",
            instId, cpu->inflyInsts.size());
    cpu->inflyInsts.erase(instId);
    this->instsCommitted[cpu->thread_context->threadId()]++;
    this->opsCommitted[cpu->thread_context->threadId()] +=
        cpu->dynamicInsts[instId]->getNumMicroOps();
  }

  this->signal->stall = this->commitQueue.size() >= this->commitQueueSize;
  if (this->signal->stall) {
    this->blockedCycles++;
  }
}
