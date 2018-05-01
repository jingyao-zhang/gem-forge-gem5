#include "cpu/llvm_trace/llvm_fetch_stage.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMFetchStage::LLVMFetchStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu)
    : cpu(_cpu),
      fetchWidth(params->fetchWidth),
      toDecodeDelay(params->fetchToDecodeDelay),
      predictor(new LLVMBranchPredictor()),
      branchPreictPenalityCycles(0) {}

LLVMFetchStage::~LLVMFetchStage() {
  delete this->predictor;
  this->predictor = nullptr;
}

void LLVMFetchStage::setToDecode(TimeBuffer<FetchStruct>* toDecodeBuffer) {
  this->toDecode = toDecodeBuffer->getWire(0);
}

void LLVMFetchStage::setSignal(TimeBuffer<LLVMStageSignal>* signalBuffer,
                               int pos) {
  this->signal = signalBuffer->getWire(pos);
}

void LLVMFetchStage::regStats() {
  this->blockedCycles.name(cpu->name() + ".fetch.blockedCycles")
      .desc("Number of cycles blocked")
      .prereq(this->blockedCycles);
}

void LLVMFetchStage::tick() {
  // If stall signal is raised, we don't fetch.
  if (this->signal->stall) {
    this->blockedCycles++;
    // DPRINTF(LLVMTraceCPU, "Fetch blocked.\n");
    return;
  }

  // If we are blocked by a wrong branch prediction,
  // we don't fetch but try to check if the conditiona branch
  // is computed out.
  if (this->branchPreictPenalityCycles > 0) {
    this->blockedCycles++;
    if (cpu->isInstFinished(this->blockedInstId)) {
      this->branchPreictPenalityCycles--;
    }
    return;
  }

  // Only fetch if the stack depth is > 0,
  // and we haven't reach fetch width,
  // and when we have more dynamic inst to fetch.
  unsigned fetchedInsts = 0;
  while (cpu->currentInstId < cpu->dynamicInsts.size() &&
         fetchedInsts < this->fetchWidth && cpu->currentStackDepth > 0) {
    LLVMDynamicInstId instId = cpu->currentInstId;
    auto inst = cpu->dynamicInsts[instId];

    // Speciall rule to skip the phi node.
    if (inst->getInstName() != "phi") {
      if (fetchedInsts + cpu->dynamicInsts[instId]->getQueueWeight() >
          this->fetchWidth) {
        // Do not fetch if overflow.
        break;
      }

      DPRINTF(LLVMTraceCPU,
              "Fetch inst %d into fetchQueue, current inst id %d, total %d\n",
              instId, cpu->currentInstId, cpu->dynamicInsts.size());
      cpu->inflyInsts[instId] = InstStatus::FETCHED;
      this->toDecode->push_back(instId);
      // Update the stack depth for call/ret inst.
      cpu->currentStackDepth +=
          cpu->dynamicInsts[instId]->getCallStackAdjustment();
      DPRINTF(LLVMTraceCPU, "Stack depth updated to %u\n",
              cpu->currentStackDepth);
      if (cpu->currentStackDepth < 0) {
        panic("Current stack depth is less than 0\n");
      }
      fetchedInsts += cpu->dynamicInsts[instId]->getQueueWeight();
    }

    cpu->currentInstId++;

    // Check if this is a conditional branch.
    if (inst->isConditionalBranchInst()) {
      bool predictionRight = this->predictor->predictAndUpdate(inst);
      if (!predictionRight) {
        DPRINTF(LLVMTraceCPU,
                "Fetch blocked due to failed branch predictor for %s.\n",
                inst->getInstName().c_str());
        this->branchPreictPenalityCycles = 8;
        this->blockedInstId = instId;
        // Do not fetch next one.
        break;
      }
    }
  }
}
