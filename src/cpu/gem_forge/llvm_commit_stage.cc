#include "cpu/gem_forge/llvm_commit_stage.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPUCommit.hh"

namespace gem5 {

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMCommitStage::LLVMCommitStage(const LLVMTraceCPUParams *params,
                                 LLVMTraceCPU *_cpu)
    : cpu(_cpu), commitWidth(params->commitWidth),
      maxCommitQueueSize(params->commitQueueSize),
      fromIEWDelay(params->iewToCommitDelay),
      commitStates(params->hardwareContexts) {}

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
  for (auto iter = this->fromIEW->begin(), end = this->fromIEW->end();
       iter != end; ++iter) {
    auto instId = *iter;

    this->commitQueue.push_back(instId);

    // Update the context.
    auto thread = cpu->inflyInstThread.at(instId);
    auto threadId = thread->getThreadId();
    this->commitStates.at(threadId).commitQueueSize++;
  }

  unsigned committedInsts = 0;
  while (!this->commitQueue.empty() && committedInsts < this->commitWidth) {
    auto instId = this->commitQueue.front();
    auto inst = cpu->inflyInstMap.at(instId);

    if (committedInsts + inst->getQueueWeight() > this->commitWidth) {
      break;
    }

    panic_if(cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end(),
             "Inst %u should be in inflyInstStatus to be commited\n", instId);

    auto instStatus = cpu->inflyInstStatus.at(instId);
    if (instStatus == InstStatus::COMMIT) {
      // First time, commit this instruction.
      cpu->iewStage.commitInst(instId);
    }

    // Get the updated status.
    instStatus = cpu->inflyInstStatus.at(instId);
    if (instStatus == InstStatus::COMMITTING) {
      // Still working, break.
      break;
    } else if (instStatus == InstStatus::COMMITTED) {
      // Done, we can move forward.

    } else {
      panic("Illegal commit status here %d for inst %lu.\n", instStatus,
            instId);
    }

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

    /**
     * For the SMT support, so far we allow it to commit from
     * any thread, but in the total order of the ROB.
     *
     * TODO: May be commit in the order of each thread.
     */
    // Any instruction specific operation when committed.
    cpu->iewStage.postCommitInst(instId);
    inst->commit(cpu);

    committedInsts += inst->getQueueWeight();
    this->commitQueue.pop_front();
    // Update the context.
    auto commitThread = cpu->inflyInstThread.at(instId);
    auto commitThreadId = commitThread->getThreadId();
    this->commitStates.at(commitThreadId).commitQueueSize--;

    // After this point, inst is released!
    cpu->inflyInstMap.erase(instId);
    cpu->inflyInstStatus.erase(instId);
    cpu->inflyInstThread.erase(instId);
    commitThread->commit(inst);

    // If the thread is done, deactivate it.
    if (commitThread->isDone()) {
      cpu->deactivateThread(commitThread);
    }
  }

  /**
   * Set the stall signal.
   * First Clear it.
   */
  for (auto &stall : this->signal->contextStall) {
    stall = false;
  }
  auto numActiveThreads = cpu->getNumActiveThreads();
  if (numActiveThreads == 0) {
    return;
  }
  auto perContextCommitQueueLimit = this->maxCommitQueueSize / numActiveThreads;
  for (ContextID contextId = 0; contextId < this->commitStates.size();
       ++contextId) {
    bool stalled = false;
    auto thread = cpu->activeThreads.at(contextId);
    if (thread == nullptr) {
      // This context is not active.
      stalled = false;
    } else {
      // Check the per-context limit.
      auto commitQueueSize = this->commitStates.at(contextId).commitQueueSize;
      if (commitQueueSize >= perContextCommitQueueLimit) {
        stalled = true;
      }
      // Check the total limit.
      if (this->commitQueue.size() >= this->maxCommitQueueSize) {
        stalled = true;
      }
    }
    this->signal->contextStall[contextId] = stalled;
  }
}

void LLVMCommitStage::clearThread(ThreadID threadId) {
  this->commitStates.at(threadId).clear();
}
} // namespace gem5
