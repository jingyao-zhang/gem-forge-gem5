#include "cpu/llvm_trace/llvm_fetch_stage.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMFetchStage::LLVMFetchStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu)
    : cpu(_cpu),
      fetchWidth(params->fetchWidth),
      toDecodeDelay(params->fetchToDecodeDelay) {}

void LLVMFetchStage::setToDecode(TimeBuffer<FetchStruct>* toDecodeBuffer) {
  this->toDecode = toDecodeBuffer->getWire(0);
}

void LLVMFetchStage::setSignal(TimeBuffer<LLVMStageSignal>* signalBuffer, int pos) {
  this->signal = signalBuffer->getWire(pos);
}

void LLVMFetchStage::regStats() {}

void LLVMFetchStage::tick() {
  // If stall signal is raised, we don't fetch.
  if (this->signal->stall) {
    return;
  }
  // Only fetch if the stack depth is > 0,
  // and we haven't reach fetch width,
  // and when we have more dynamic inst to fetch.
  unsigned fetchedInsts = 0;
  while (cpu->currentInstId < cpu->dynamicInsts.size() &&
         fetchedInsts < this->fetchWidth && cpu->currentStackDepth > 0) {
    DPRINTF(LLVMTraceCPU, "Fetch inst %d into fetchQueue\n",
            cpu->currentInstId);
    cpu->inflyInsts[cpu->currentInstId] = InstStatus::FETCHED;
    this->toDecode->push_back(cpu->currentInstId);
    // Update the stack depth for call/ret inst.
    cpu->currentStackDepth +=
        cpu->dynamicInsts[cpu->currentInstId]->getCallStackAdjustment();
    DPRINTF(LLVMTraceCPU, "Stack depth updated to %u\n",
            cpu->currentStackDepth);
    if (cpu->currentStackDepth < 0) {
      panic("Current stack depth is less than 0\n");
    }
    cpu->currentInstId++;
    fetchedInsts++;
  }
}
