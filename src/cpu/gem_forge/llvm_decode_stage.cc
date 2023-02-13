#include "cpu/gem_forge/llvm_decode_stage.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

namespace gem5 {

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMDecodeStage::LLVMDecodeStage(const LLVMTraceCPUParams *params,
                                 LLVMTraceCPU *_cpu)
    : cpu(_cpu), decodeWidth(params->decodeWidth),
      maxDecodeQueueSize(params->decodeQueueSize),
      fromFetchDelay(params->fetchToDecodeDelay),
      toRenameDelay(params->decodeToRenameDelay),
      decodeStates(params->hardwareContexts), lastDecodedThreadId(0) {}

void LLVMDecodeStage::setToRename(TimeBuffer<FetchStruct> *toRenameBuffer) {
  this->toRename = toRenameBuffer->getWire(0);
}

void LLVMDecodeStage::setFromFetch(TimeBuffer<FetchStruct> *fromFetchBuffer) {
  this->fromFetch = fromFetchBuffer->getWire(-this->fromFetchDelay);
}

void LLVMDecodeStage::setSignal(TimeBuffer<LLVMStageSignal> *signalBuffer,
                                int pos) {
  this->signal = signalBuffer->getWire(pos);
}

std::string LLVMDecodeStage::name() { return cpu->name() + ".decode"; }

void LLVMDecodeStage::regStats() {
  this->blockedCycles.name(name() + ".blockedCycles")
      .desc("Number of cycles blocked")
      .prereq(this->blockedCycles);
  decodeDecodedInsts.name(name() + ".DecodedInsts")
      .desc("Number of instructions handled by decode")
      .prereq(this->decodeDecodedInsts);
}

void LLVMDecodeStage::tick() {
  // Get fetched inst from fetch.
  for (auto iter = this->fromFetch->begin(), end = this->fromFetch->end();
       iter != end; iter++) {
    auto instId = *iter;
    auto threadId = cpu->inflyInstThread.at(instId)->getThreadId();
    this->decodeStates.at(threadId).decodeQueue.push(instId);
  }

  // Round-robin to find next non-blocking thread to decode.
  LLVMTraceThreadContext *chosenThread = nullptr;
  ThreadID chosenThreadId = 0;
  for (auto offset = 1; offset <= this->decodeStates.size() + 1; ++offset) {
    auto threadId =
        (this->lastDecodedThreadId + offset) % this->decodeStates.size();
    auto thread = cpu->activeThreads[threadId];
    if (thread == nullptr) {
      // This context id is not allocated.
      continue;
    }
    if (this->signal->contextStall[threadId]) {
      // The next stage require this context to be stalled.
      continue;
    }
    chosenThreadId = threadId;
    chosenThread = thread;
    break;
  }

  if (chosenThread == nullptr) {
    this->blockedCycles++;
    return;
  }

  this->lastDecodedThreadId = chosenThreadId;

  // Decode from the queue for the chosen context.
  // Only decode if we haven't reach decode width and we have more inst to
  // decode.
  unsigned decodedInsts = 0;
  auto &decodeQueue = this->decodeStates.at(chosenThreadId).decodeQueue;
  while (decodedInsts < this->decodeWidth && !decodeQueue.empty()) {
    auto instId = decodeQueue.front();

    auto inst = cpu->inflyInstMap.at(instId);

    if (decodedInsts + inst->getQueueWeight() > this->decodeWidth) {
      break;
    }

    decodeQueue.pop();
    DPRINTF(LLVMTraceCPU, "Decode inst %d\n", instId);
    cpu->inflyInstStatus.at(instId) = InstStatus::DECODED;
    this->toRename->push_back(instId);
    decodedInsts += inst->getQueueWeight();
  }

  this->decodeDecodedInsts += decodedInsts;

  // Clear and then set the stall signal.
  for (auto &stall : this->signal->contextStall) {
    stall = false;
  }
  auto numActiveNonIdealThreads = cpu->getNumActiveNonIdealThreads();
  if (numActiveNonIdealThreads == 0) {
    return;
  }
  auto totalDecodeQueueSize = this->getTotalDecodeQueueSize();
  auto perContextDecodeQueueLimit =
      this->maxDecodeQueueSize / numActiveNonIdealThreads;
  for (ContextID contextId = 0; contextId < this->decodeStates.size();
       ++contextId) {
    bool stalled = false;
    auto thread = cpu->activeThreads.at(contextId);
    if (thread == nullptr) {
      // This context is not active.
      stalled = false;
    } else if (thread->isIdealThread()) {
      // Never stalled ideal thread.
      stalled = false;
    } else {
      // Check the per-context limit.
      auto decodeQueueSize =
          this->decodeStates.at(contextId).decodeQueue.size();
      if (decodeQueueSize >= perContextDecodeQueueLimit) {
        stalled = true;
      }
      // Check the total limit.
      if (totalDecodeQueueSize >= this->maxDecodeQueueSize) {
        stalled = true;
      }
    }
    this->signal->contextStall[contextId] = stalled;
  }
}

size_t LLVMDecodeStage::getTotalDecodeQueueSize() const {
  size_t totalDecodeQueueSize = 0;
  for (ContextID contextId = 0; contextId < this->decodeStates.size();
       ++contextId) {
    auto thread = cpu->activeThreads.at(contextId);
    if (thread == nullptr) {
      // This context is not active.
      continue;
    } else if (thread->isIdealThread()) {
      // Ignore ideal thread.
      continue;
    } else {
      // Check the per-context limit.
      auto decodeQueueSize =
          this->decodeStates.at(contextId).decodeQueue.size();
      totalDecodeQueueSize += decodeQueueSize;
    }
  }
  return totalDecodeQueueSize;
}
} // namespace gem5
