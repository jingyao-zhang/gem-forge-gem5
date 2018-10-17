#include "cpu/llvm_trace/llvm_commit_stage.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPUCommit.hh"

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

std::string LLVMCommitStage::name() { return cpu->name() + ".commit"; }

void LLVMCommitStage::regStats() {
  // No stats for now.
  int nThreads = cpu->numThreads;
  if (cpu->isStandalone()) {
    nThreads = 1;
  }
  this->instsCommitted.init(nThreads)
      .name(name() + ".committedInsts")
      .desc("Number of instructions committed")
      .flags(Stats::total);
  this->opsCommitted.init(nThreads)
      .name(name() + ".committedOps")
      .desc("Number of ops (including micro ops) committed")
      .flags(Stats::total);
  this->intInstsCommitted.init(nThreads)
      .name(name() + ".committedIntInsts")
      .desc("Number of integer instructions committed")
      .flags(Stats::total);
  this->fpInstsCommitted.init(nThreads)
      .name(name() + ".committedFpInsts")
      .desc("Number of float instructions committed")
      .flags(Stats::total);
  this->callInstsCommitted.init(nThreads)
      .name(name() + ".committedCallInsts")
      .desc("Number of call instructions committed")
      .flags(Stats::total);

  this->blockedCycles.name(name() + ".blockedCycles")
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

    DPRINTF(LLVMTraceCPUCommit,
            "Inst %lu committed, remaining infly inst #%u\n", inst->getSeqNum(),
            cpu->inflyInstStatus.size());

    int threadId = 0;
    if (!cpu->isStandalone()) {
      threadId = cpu->thread_context->threadId();
    }
    this->instsCommitted[threadId]++;
    this->opsCommitted[threadId] += inst->getNumMicroOps();
    if (inst->isFloatInst()) {
      this->fpInstsCommitted[threadId]++;
    } else {
      this->intInstsCommitted[threadId]++;
    }
    if (inst->isCallInst()) {
      this->callInstsCommitted[threadId]++;
    }
    // Any instruction specific operation when committed.
    inst->commit(cpu);

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
