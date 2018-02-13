#include "cpu/llvm_trace/llvm_rename_stage.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMRenameStage::LLVMRenameStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu)
    : cpu(_cpu),
      renameWidth(params->renameWidth),
      robSize(params->robSize),
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

void LLVMRenameStage::regStats() {}

void LLVMRenameStage::tick() {
  // Get the inst from decode to rob;
  for (auto iter = this->fromDecode->begin(), end = this->fromDecode->end();
       iter != end; ++iter) {
    this->rob.push_back(*iter);
  }

  // Check the dependence until either the rob is empty or
  // we have reached the renameWidth.
  if (!this->signal->stall) {
    unsigned renamedInsts = 0;
    auto robIter = this->rob.begin();
    while (renamedInsts < this->renameWidth && robIter != this->rob.end()) {
      auto instId = *robIter;
      auto inst = cpu->dynamicInsts[instId];
      // Sanity check.
      panic_if(cpu->inflyInsts.find(instId) == cpu->inflyInsts.end(),
               "Inst %u should be in inflyInsts to check if READY\n", instId);
      if (cpu->inflyInsts.at(instId) != InstStatus::DECODED) {
        panic("Inst %u should be in DECODED status to check if READY\n",
              instId);
      }

      // Check if ready.
      if (inst->isDependenceReady(cpu)) {
        // Mark the status to ready.
        DPRINTF(LLVMTraceCPU,
                "Inst %u is ready and send to instruction queue.\n", instId);
        cpu->inflyInsts[instId] = InstStatus::READY;

        // Add toIEW.
        this->toIEW->push_back(instId);

        renamedInsts++;

        // Remove the inst from rob.
        robIter = this->rob.erase(robIter);
        continue;
      }

      robIter++;
    }
  }

  // Raise stall signal to decode if rob size has reached limits.
  this->signal->stall = this->rob.size() >= this->robSize;
}
