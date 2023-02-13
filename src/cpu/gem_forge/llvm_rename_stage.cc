#include "cpu/gem_forge/llvm_rename_stage.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

namespace gem5 {

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMRenameStage::LLVMRenameStage(const LLVMTraceCPUParams *params,
                                 LLVMTraceCPU *_cpu)
    : cpu(_cpu), renameWidth(params->renameWidth),
      maxRenameQueueSize(params->renameBufferSize),
      fromDecodeDelay(params->decodeToRenameDelay),
      toIEWDelay(params->renameToIEWDelay),
      renameStates(params->hardwareContexts), lastRenamedThreadId(0) {}

void LLVMRenameStage::setToIEW(TimeBuffer<RenameStruct> *toIEWBuffer) {
  this->toIEW = toIEWBuffer->getWire(0);
}

void LLVMRenameStage::setFromDecode(
    TimeBuffer<DecodeStruct> *fromDecodeBuffer) {
  this->fromDecode = fromDecodeBuffer->getWire(-this->fromDecodeDelay);
}

void LLVMRenameStage::setSignal(TimeBuffer<LLVMStageSignal> *signalBuffer,
                                int pos) {
  this->signal = signalBuffer->getWire(pos);
}

std::string LLVMRenameStage::name() { return cpu->name() + ".rename"; }

void LLVMRenameStage::regStats() {
  this->blockedCycles.name(name() + ".blockedCycles")
      .desc("Number of cycles blocked")
      .prereq(this->blockedCycles);
  renameRenamedInsts.name(name() + ".RenamedInsts")
      .desc("Number of instructions processed by rename")
      .prereq(renameRenamedInsts);
  renameRenamedOperands.name(name() + ".RenamedOperands")
      .desc("Number of destination operands rename has renamed")
      .prereq(renameRenamedOperands);
  renameRenameLookups.name(name() + ".RenameLookups")
      .desc("Number of register rename lookups that rename has made")
      .prereq(renameRenameLookups);
  intRenameLookups.name(name() + ".int_rename_lookups")
      .desc("Number of integer rename lookups")
      .prereq(intRenameLookups);
  fpRenameLookups.name(name() + ".fp_rename_lookups")
      .desc("Number of floating rename lookups")
      .prereq(fpRenameLookups);
  vecRenameLookups.name(name() + ".vec_rename_lookups")
      .desc("Number of vector rename lookups")
      .prereq(vecRenameLookups);
}

void LLVMRenameStage::tick() {
  // Get the inst from decode to rename queue;
  for (auto iter = this->fromDecode->begin(), end = this->fromDecode->end();
       iter != end; ++iter) {
    auto instId = *iter;
    auto threadId = cpu->inflyInstThread.at(instId)->getThreadId();
    this->renameStates.at(threadId).renameQueue.push(instId);
  }

  // Round-robin to find next non-blocking thread to rename.
  LLVMTraceThreadContext *chosenThread = nullptr;
  ContextID chosenContextId = 0;
  for (auto offset = 1; offset <= this->renameStates.size(); ++offset) {
    auto threadId =
        (this->lastRenamedThreadId + offset) % this->renameStates.size();
    auto thread = cpu->activeThreads[threadId];
    if (thread == nullptr) {
      // This context id is not allocated.
      continue;
    }
    if (this->signal->contextStall[threadId]) {
      // The next stage require this context to be stalled.
      continue;
    }
    chosenContextId = threadId;
    chosenThread = thread;
    break;
  }

  if (chosenThread == nullptr) {
    this->blockedCycles++;
    return;
  }

  this->lastRenamedThreadId = chosenContextId;

  unsigned renamedInsts = 0;
  unsigned renamedIntDest = 0;
  unsigned renamedFpDest = 0;
  unsigned renamedIntLookUp = 0;
  unsigned renamedFpLookUp = 0;
  auto &renameQueue = this->renameStates.at(chosenContextId).renameQueue;
  while (renamedInsts < this->renameWidth && !renameQueue.empty()) {
    auto instId = renameQueue.front();
    auto &inst = cpu->inflyInstMap.at(instId);
    // Sanity check.
    if (cpu->inflyInstStatus.at(instId) != InstStatus::DECODED) {
      panic("Inst %u should be in DECODED status in rename queue\n", instId);
    }

    if (renamedInsts + inst->getQueueWeight() > this->renameWidth) {
      break;
    }

    DPRINTF(LLVMTraceCPU, "Inst %u is sent to iew.\n", instId);

    // Add toIEW.
    this->toIEW->push_back(instId);

    renamedInsts += inst->getQueueWeight();

    if (inst->isFloatInst()) {
      renamedFpDest += inst->getNumResults();
      renamedFpLookUp += inst->getNumOperands();
    } else {
      renamedIntDest += inst->getNumResults();
      renamedIntLookUp += inst->getNumOperands();
    }

    // Remove the inst from renameQueue
    renameQueue.pop();
  }

  this->renameRenamedInsts += renamedInsts;
  this->renameRenamedOperands += renamedIntDest + renamedFpDest;
  this->renameRenameLookups += renamedIntLookUp + renamedFpLookUp;
  this->intRenameLookups += renamedIntLookUp;
  this->fpRenameLookups += renamedFpLookUp;

  // Clear and then set the stall signal.
  for (auto &stall : this->signal->contextStall) {
    stall = false;
  }
  auto numActiveNonIdealThreads = cpu->getNumActiveNonIdealThreads();
  if (numActiveNonIdealThreads == 0) {
    return;
  }
  auto totalRenameQueueSize = this->getTotalRenameQueueSize();
  auto perContextRenameQueueLimit =
      this->maxRenameQueueSize / numActiveNonIdealThreads;
  for (ContextID threadId = 0; threadId < this->renameStates.size();
       ++threadId) {
    bool stalled = false;
    auto thread = cpu->activeThreads.at(threadId);
    if (thread == nullptr) {
      // This context is not active.
      stalled = false;
    } else if (thread->isIdealThread()) {
      stalled = false;
    } else {
      // Check the per-context limit.
      auto renameQueueSize = this->renameStates.at(threadId).renameQueue.size();
      if (renameQueueSize >= perContextRenameQueueLimit) {
        stalled = true;
      }
      // Check the total limit.
      if (totalRenameQueueSize >= this->maxRenameQueueSize) {
        stalled = true;
      }
    }
    this->signal->contextStall[threadId] = stalled;
  }
}

size_t LLVMRenameStage::getTotalRenameQueueSize() const {
  size_t totalRenameQueueSize = 0;
  for (ContextID threadId = 0; threadId < this->renameStates.size();
       ++threadId) {
    auto thread = cpu->activeThreads.at(threadId);
    if (thread == nullptr) {
      // This context is not active.
      continue;
    } else if (thread->isIdealThread()) {
      continue;
    } else {
      // Check the per-context limit.
      auto renameQueueSize = this->renameStates.at(threadId).renameQueue.size();
      totalRenameQueueSize += renameQueueSize;
    }
  }
  return totalRenameQueueSize;
}
} // namespace gem5
