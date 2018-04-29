#include "cpu/llvm_trace/llvm_rename_stage.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMRenameStage::LLVMRenameStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu)
    : cpu(_cpu),
      renameWidth(params->renameWidth),
      renameBufferSize(params->renameBufferSize),
      fromDecodeDelay(params->decodeToRenameDelay),
      toIEWDelay(params->renameToIEWDelay) {}

void LLVMRenameStage::setToIEW(TimeBuffer<RenameStruct>* toIEWBuffer) {
  this->toIEW = toIEWBuffer->getWire(0);
}

void LLVMRenameStage::setFromDecode(
    TimeBuffer<DecodeStruct>* fromDecodeBuffer) {
  this->fromDecode = fromDecodeBuffer->getWire(-this->fromDecodeDelay);
}

void LLVMRenameStage::setSignal(TimeBuffer<LLVMStageSignal>* signalBuffer,
                                int pos) {
  this->signal = signalBuffer->getWire(pos);
}

void LLVMRenameStage::regStats() {
  this->blockedCycles.name(cpu->name() + ".rename.blockedCycles")
      .desc("Number of cycles blocked")
      .prereq(this->blockedCycles);
}

void LLVMRenameStage::tick() {
  // Get the inst from decode to rob;
  for (auto iter = this->fromDecode->begin(), end = this->fromDecode->end();
       iter != end; ++iter) {
    this->renameBuffer.push_back(*iter);
  }

  if (this->signal->stall) {
    this->blockedCycles++;
  }

  // Check the dependence until either the rob is empty or
  // we have reached the renameWidth.
  if (!this->signal->stall) {
    unsigned renamedInsts = 0;
    auto iter = this->renameBuffer.begin();
    while (renamedInsts < this->renameWidth && iter != this->renameBuffer.end()) {
      auto instId = *iter;
      auto inst = cpu->dynamicInsts[instId];
      // Sanity check.
      panic_if(cpu->inflyInsts.find(instId) == cpu->inflyInsts.end(),
               "Inst %u should be in inflyInsts to check if READY\n", instId);
      if (cpu->inflyInsts.at(instId) != InstStatus::DECODED) {
        panic("Inst %u should be in DECODED status in rob\n", instId);
      }

      if (renamedInsts + inst->getQueueWeight() > this->renameWidth) {
        break;
      }

      DPRINTF(LLVMTraceCPU, "Inst %u is sent to iew.\n", instId);

      // Add toIEW.
      this->toIEW->push_back(instId);

      renamedInsts += inst->getQueueWeight();

      // Remove the inst from renameBuffer.
      iter = this->renameBuffer.erase(iter);
    }
  }

  // Raise stall signal to decode if rob size has reached limits.
  this->signal->stall = this->renameBuffer.size() >= this->renameBufferSize;
}
