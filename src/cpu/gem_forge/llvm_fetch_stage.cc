#include "cpu/gem_forge/llvm_fetch_stage.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPUFetch.hh"

namespace gem5 {

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMFetchStage::LLVMFetchStage(const LLVMTraceCPUParams *params,
                               LLVMTraceCPU *_cpu)
    : cpu(_cpu), fetchWidth(params->fetchWidth),
      fetchStates(params->hardwareContexts),
      toDecodeDelay(params->fetchToDecodeDelay),
      useGem5BranchPredictor(params->useGem5BranchPredictor),
      predictor(new LLVMBranchPredictor()), branchPredictor(params->branchPred),
      lastFetchedThreadId(0) {}

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
  this->branchPredMissesLLVM.name(name() + ".branchPredMissesLLVM")
      .desc("Number of branch prediction misses by our simple LLVM BP.")
      .prereq(this->branchPredMissesLLVM);
  this->branchPredMissesGem5.name(name() + ".branchPredMissesGem5")
      .desc("Number of branch prediction misses by Gem5 BP.")
      .prereq(this->branchPredMissesGem5);

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

void LLVMFetchStage::clearThread(ThreadID threadId) {
  this->fetchStates[threadId].clear();
}

void LLVMFetchStage::tick() {

  // Check all active threads.
  LLVMTraceThreadContext *chosenThread = nullptr;
  int chosenContextId = 0;
  for (auto offset = 1; offset <= this->fetchStates.size() + 1; ++offset) {
    auto threadId =
        (this->lastFetchedThreadId + offset) % this->fetchStates.size();
    auto thread = cpu->activeThreads[threadId];
    if (thread == nullptr) {
      // This context id is not allocated.
      continue;
    }
    if (!thread->canFetch()) {
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

  this->lastFetchedThreadId = chosenContextId;

  auto &fetchState = this->fetchStates[chosenContextId];
  // If we are blocked by a wrong branch prediction,
  // we don't fetch but try to check if the conditional branch
  // is computed out.
  if (fetchState.branchPredictPenalityCycles > 0) {
    this->blockedCycles++;
    if (cpu->isInstFinished(fetchState.blockedInstId)) {
      fetchState.branchPredictPenalityCycles--;
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

      DPRINTF(LLVMTraceCPUFetch,
              "Fetch inst %lu %s into fetchQueue, infly insts %lu.\n",
              inst->getSeqNum(), inst->getTDG().op().c_str(),
              cpu->inflyInstMap.size());
      // Update the infly.
      cpu->inflyInstStatus[instId] = InstStatus::FETCHED;
      cpu->inflyInstMap.emplace(instId, inst);
      cpu->inflyInstThread.emplace(instId, chosenThread);
      // Send to decode.
      this->toDecode->push_back(instId);

      // Update the serializeAfter flag.
      if (fetchState.serializeAfter) {
        inst->markSerializeBefore();
        fetchState.serializeAfter = false;
      }
      if (inst->isSerializeAfter()) {
        fetchState.serializeAfter = true;
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

    {
      // Branch prediction.
      if (inst->isBranchInst()) {
        this->branchInsts++;
        this->fetchedBranches++;

        // Check if this is a branch.
        bool predictionWrongLLVM = !this->predictor->predictAndUpdate(inst);
        if (predictionWrongLLVM) {
          this->branchPredMissesLLVM++;
        }

        /**
         * In order to use gem5's branch prediction, we create the
         * data structure it requires.
         */
        auto instSeqNum = inst->getSeqNum();
        auto threadId = chosenThread->getThreadId();
        auto staticInst = inst->getStaticInst();
        TheISA::PCState targetPCState(inst->getPC());
        auto predictTaken = this->branchPredictor->predict(
            staticInst, instSeqNum, targetPCState, threadId);

        /**
         * Validate the prediction result.
         *
         * Notice that for ret, it relies on the RAS to predict the target,
         * which essentially boils down to advance the caller's pc.
         * However, so far our fake PCState does not now how to advance the pc,
         * which makes all the prediction result for ret wrong.
         *
         * Since RAS is very accurate, for ret we ignore the result from branch
         * prediction and simply assume it's always correct.
         */
        TheISA::PCState dynamicNextPCState(inst->getDynamicNextPC());

        bool predictWrongGem5 = dynamicNextPCState.pc() != targetPCState.pc() &&
                                inst->getInstName() != "ret";

        if (predictWrongGem5) {
          // Prediction wrong.
          this->branchPredMissesGem5++;
          // Notify the predictor via squashing.
          this->branchPredictor->squash(instSeqNum, dynamicNextPCState,
                                        !predictTaken, threadId);
        }
        // For simplicity, we always commit immediately.
        this->branchPredictor->update(instSeqNum, threadId);

        auto predictWrong = this->useGem5BranchPredictor ? predictWrongGem5
                                                         : predictionWrongLLVM;
        if (predictWrong) {
          DPRINTF(LLVMTraceCPUFetch,
                  "Fetch blocked due to failed branch predictor for %s.\n",
                  inst->getInstName().c_str());
          fetchState.branchPredictPenalityCycles = 8;
          fetchState.blockedInstId = instId;
          this->branchPredMisses++;
          // Do not fetch next one.
          break;
        }
      }
    }
  }

  this->fetchedInsts += fetchedInsts;
}
} // namespace gem5
