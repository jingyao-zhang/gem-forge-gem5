#include "cpu/gem_forge/llvm_decode_stage.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMDecodeStage::LLVMDecodeStage(LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu)
    : cpu(_cpu), decodeWidth(params->decodeWidth),
      maxDecodeQueueSize(params->decodeQueueSize),
      fromFetchDelay(params->fetchToDecodeDelay),
      toRenameDelay(params->decodeToRenameDelay),
      decodeStates(params->hardwareContexts), totalDecodeQueueSize(0),
      lastDecodedContextId(0) {}

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
    auto contextId = cpu->inflyInstThread.at(instId)->getContextId();
    this->decodeStates.at(contextId).decodeQueue.push(instId);
    this->totalDecodeQueueSize++;
  }

  // Round-robin to find next non-blocking thread to decode.
  LLVMTraceThreadContext *chosenThread = nullptr;
  ThreadID chosenContextId = 0;
  for (auto offset = 1; offset <= this->decodeStates.size() + 1; ++offset) {
    auto contextId =
        (this->lastDecodedContextId + offset) % this->decodeStates.size();
    auto thread = cpu->activeThreads[contextId];
    if (thread == nullptr) {
      // This context id is not allocated.
      continue;
    }
    if (this->signal->contextStall[contextId]) {
      // The next stage require this context to be stalled.
      continue;
    }
    chosenContextId = contextId;
    chosenThread = thread;
    break;
  }

  if (chosenThread == nullptr) {
    this->blockedCycles++;
    return;
  }

  this->lastDecodedContextId = chosenContextId;

  // Decode from the queue for the chosen context.
  // Only decode if we haven't reach decode width and we have more inst to
  // decode.
  unsigned decodedInsts = 0;
  auto &decodeQueue = this->decodeStates.at(chosenContextId).decodeQueue;
  while (decodedInsts < this->decodeWidth && !decodeQueue.empty()) {
    auto instId = decodeQueue.front();

    auto inst = cpu->inflyInstMap.at(instId);

    if (decodedInsts + inst->getQueueWeight() > this->decodeWidth) {
      break;
    }

    decodeQueue.pop();
    this->totalDecodeQueueSize--;
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
  auto numActiveThreads = cpu->getNumActivateThreads();
  if (numActiveThreads == 0) {
    return;
  }
  auto perContextDecodeQueueLimit = this->maxDecodeQueueSize / numActiveThreads;
  for (ThreadID contextId = 0; contextId < this->decodeStates.size();
       ++contextId) {
    bool stalled = false;
    auto thread = cpu->activeThreads.at(contextId);
    if (thread == nullptr) {
      // This context is not active.
      stalled = false;
    } else {
      // Check the per-context limit.
      auto decodeQueueSize =
          this->decodeStates.at(contextId).decodeQueue.size();
      if (decodeQueueSize >= perContextDecodeQueueLimit) {
        stalled = true;
      }
      // Check the total limit.
      if (this->totalDecodeQueueSize >= this->maxDecodeQueueSize) {
        stalled = true;
      }
    }
    this->signal->contextStall[contextId] = stalled;
  }
}
