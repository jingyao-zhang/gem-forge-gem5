#include "cpu/llvm_trace/llvm_fetch_stage.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPUFetch.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMFetchStage::LLVMFetchStage(LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu)
    : cpu(_cpu), fetchWidth(params->fetchWidth), threadStates(4), SMTLimit(4),
      toDecodeDelay(params->fetchToDecodeDelay),
      predictor(new LLVMBranchPredictor()), lastFetchedHWTD(3) {}

LLVMFetchStage::~LLVMFetchStage() {
  delete this->predictor;
  this->predictor = nullptr;
}

std::string LLVMFetchStage::name() { return cpu->name() + ".fetch"; }

void LLVMFetchStage::setToDecode(TimeBuffer<FetchStruct> *toDecodeBuffer) {
  this->toDecode = toDecodeBuffer->getWire(0);
}

void LLVMFetchStage::setSignal(TimeBuffer<LLVMStageSignal> *signalBuffer,
                               int pos) {
  this->signal = signalBuffer->getWire(pos);
}

void LLVMFetchStage::regStats() {
  this->blockedCycles.name(name() + ".blockedCycles")
      .desc("Number of cycles blocked")
      .prereq(this->blockedCycles);
  this->branchInsts.name(name() + ".branchInsts")
      .desc("Number of branches")
      .prereq(this->branchInsts);
  this->branchPredMisses.name(name() + ".branchPredMisses")
      .desc("Number of branch prediction misses")
      .prereq(this->branchPredMisses);

  this->fetchedInsts.name(name() + ".Insts")
      .desc("Number of instructions fetch has processed")
      .prereq(fetchedInsts);

  this->fetchedBranches.name(name() + ".Branches")
      .desc("Number of branches that fetch encountered")
      .prereq(fetchedBranches);

  this->predictedBranches.name(name() + ".predictedBranches")
      .desc("Number of branches that fetch has predicted taken")
      .prereq(predictedBranches);
}

void LLVMFetchStage::tick() {
  // If stall signal is raised, we don't fetch.
  // TODO: extend the stall signal to stall active threads.
  if (this->signal->stall) {
    this->blockedCycles++;
    // DPRINTF(LLVMTraceCPU, "Fetch blocked.\n");
    return;
  }

  // Check all active threads.
  LLVMTraceThreadContext *chosenThread = nullptr;
  int chosenIdx = 0;
  for (auto offset = 1; offset <= this->SMTLimit + 1; ++offset) {
    auto idx = (this->lastFetchedHWTD + offset) % this->SMTLimit;
    if (idx > cpu->activeThreads.size()) {
      continue;
    }
    auto thread = cpu->activeThreads[idx];
    if (thread->canFetch()) {
      chosenIdx = 0;
      chosenThread = thread;
    }
  }

  if (chosenThread == nullptr) {
    this->blockedCycles++;
    return;
  }

  auto &states = this->threadStates[chosenIdx];
  // If we are blocked by a wrong branch prediction,
  // we don't fetch but try to check if the conditiona branch
  // is computed out.
  if (states.branchPreictPenalityCycles > 0) {
    this->blockedCycles++;
    if (cpu->isInstFinished(states.blockedInstId)) {
      states.branchPreictPenalityCycles--;
    }
    return;
  }

  // Only fetch if the stack depth is > 0,
  // and we haven't reach fetch width,
  // and when we have more dynamic inst to fetch.
  unsigned fetchedInsts = 0;
  while ((chosenThread->canFetch()) && fetchedInsts < this->fetchWidth &&
         cpu->currentStackDepth > 0) {
    auto inst = chosenThread->fetch();
    auto instId = inst->getId();

    // Speciall rule to skip the phi node.
    if (inst->getInstName() != "phi") {
      if (fetchedInsts + inst->getQueueWeight() > this->fetchWidth) {
        // Do not fetch if overflow.
        break;
      }

      DPRINTF(
          LLVMTraceCPUFetch,
          "Fetch inst %lu %s into fetchQueue, infly insts %lu,  remaining %d\n",
          inst->getSeqNum(), inst->getTDG().op().c_str(),
          cpu->inflyInstMap.size(), cpu->dynInstStream->fetchSize());
      // Update the infly.
      cpu->inflyInstStatus[instId] = InstStatus::FETCHED;
      cpu->inflyInstMap.emplace(instId, inst);
      cpu->inflyInstThread.emplace(instId, chosenThread);
      // Send to decode.
      this->toDecode->push_back(instId);

      // Update the serializeAfter flag.
      if (states.serializeAfter) {
        inst->markSerializeBefore();
        states.serializeAfter = false;
      }
      if (inst->isSerializeAfter()) {
        states.serializeAfter = true;
      }

      // Update the stack depth for call/ret inst.
      int stackAdjustment = inst->getCallStackAdjustment();
      switch (stackAdjustment) {
      case 1: {
        cpu->stackPush();
        DPRINTF(LLVMTraceCPUFetch, "Stack depth updated to %u\n",
                cpu->currentStackDepth);
        break;
      }
      case -1: {
        cpu->stackPop();
        DPRINTF(LLVMTraceCPUFetch, "Stack depth updated to %u\n",
                cpu->currentStackDepth);
        break;
      }
      case 0: {
        break;
      }
      default: {
        panic("Illegal call stack adjustment &d.\n", stackAdjustment);
      }
      }
      fetchedInsts += inst->getQueueWeight();
    }

    // Check if this is a conditional branch.
    if (inst->isConditionalBranchInst()) {
      this->branchInsts++;
      this->fetchedBranches++;
      bool predictionRight = this->predictor->predictAndUpdate(inst);
      if (!predictionRight) {
        this->branchPredMisses++;
        DPRINTF(LLVMTraceCPUFetch,
                "Fetch blocked due to failed branch predictor for %s.\n",
                inst->getInstName().c_str());
        states.branchPreictPenalityCycles = 8;
        states.blockedInstId = instId;
        // Do not fetch next one.
        break;
      }
    }
  }

  this->fetchedInsts += fetchedInsts;
}
