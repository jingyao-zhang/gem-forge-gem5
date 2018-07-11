#include "cpu/llvm_trace/llvm_commit_stage.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMCommitStage::LLVMCommitStage(LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu)
    : cpu(_cpu), commitWidth(params->commitWidth),
      commitQueueSize(params->commitQueueSize),
      fromIEWDelay(params->iewToCommitDelay) {}

void LLVMCommitStage::setFromIEW(TimeBuffer<IEWStruct> *fromIEWBuffer) {
  this->fromIEW = fromIEWBuffer->getWire(-this->fromIEWDelay);
}

void LLVMCommitStage::setSignal(TimeBuffer<LLVMStageSignal> *signalBuffer,
                                int pos) {
  this->signal = signalBuffer->getWire(pos);
}

void LLVMCommitStage::regStats() {
  // No stats for now.
  int nThreads = cpu->numThreads;
  if (cpu->isStandalone()) {
    nThreads = 1;
  }
  this->instsCommitted.init(nThreads)
      .name(cpu->name() + ".commit.committedInsts")
      .desc("Number of instructions committed")
      .flags(Stats::total);
  this->opsCommitted.init(nThreads)
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
    auto inst = cpu->inflyInstMap.at(instId);

    if (committedInsts + inst->getQueueWeight() > this->commitWidth) {
      break;
    }

    committedInsts += inst->getQueueWeight();
    this->commitQueue.pop_front();

    panic_if(cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end(),
             "Inst %u should be in inflyInstStatus to be commited\n", instId);
    panic_if(cpu->inflyInstStatus.at(instId) != InstStatus::WRITEBACKED,
             "Inst %u should be writebacked to be commited, not %d\n", instId,
             cpu->inflyInstStatus.at(instId));

    DPRINTF(LLVMTraceCPU, "Inst %u committed, remaining infly inst #%u\n",
            instId, cpu->inflyInstStatus.size());

    if (!cpu->isStandalone()) {
      this->instsCommitted[cpu->thread_context->threadId()]++;
      this->opsCommitted[cpu->thread_context->threadId()] +=
          inst->getNumMicroOps();
    } else {
      this->instsCommitted[0]++;
      this->opsCommitted[0] += inst->getNumMicroOps();
    }

    // After this point, inst is released!
    cpu->inflyInstMap.erase(instId);
    cpu->inflyInstStatus.erase(instId);
    cpu->dynInstStream->commit(inst);
  }

  this->signal->stall = this->commitQueue.size() >= this->commitQueueSize;
  if (this->signal->stall) {
    this->blockedCycles++;
  }
}
