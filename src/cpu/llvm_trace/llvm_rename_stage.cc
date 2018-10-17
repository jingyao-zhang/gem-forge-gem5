#include "cpu/llvm_trace/llvm_rename_stage.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMRenameStage::LLVMRenameStage(LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu)
    : cpu(_cpu), renameWidth(params->renameWidth),
      renameBufferSize(params->renameBufferSize),
      fromDecodeDelay(params->decodeToRenameDelay),
      toIEWDelay(params->renameToIEWDelay) {}

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
    unsigned renamedIntDest = 0;
    unsigned renamedFpDest = 0;
    unsigned renamedIntLookUp = 0;
    unsigned renamedFpLookUp = 0;
    auto iter = this->renameBuffer.begin();
    while (renamedInsts < this->renameWidth &&
           iter != this->renameBuffer.end()) {
      auto instId = *iter;
      auto &inst = cpu->inflyInstMap.at(instId);
      // Sanity check.
      panic_if(cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end(),
               "Inst %u should be in inflyInstStatus to check if READY\n",
               instId);
      if (cpu->inflyInstStatus.at(instId) != InstStatus::DECODED) {
        panic("Inst %u should be in DECODED status in rob\n", instId);
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

      // Remove the inst from renameBuffer.
      iter = this->renameBuffer.erase(iter);
    }

    this->renameRenamedInsts += renamedInsts;
    this->renameRenamedOperands += renamedIntDest + renamedFpDest;
    this->renameRenameLookups += renamedIntLookUp + renamedFpLookUp;
    this->intRenameLookups += renamedIntLookUp;
    this->fpRenameLookups += renamedFpLookUp;
  }

  // Raise stall signal to decode if rob size has reached limits.
  this->signal->stall = this->renameBuffer.size() >= this->renameBufferSize;
}
