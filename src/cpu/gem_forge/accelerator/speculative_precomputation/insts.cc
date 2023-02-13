#include "insts.hh"
#include "speculative_precomputation_manager.hh"

// #include "base/misc.hh""
#include "base/trace.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "debug/SpeculativePrecomputation.hh"

namespace gem5 {

SpeculativePrecomputationTriggerInst::SpeculativePrecomputationTriggerInst(
    const LLVM::TDG::TDGInstruction &_TDG)
    : LLVMDynamicInst(_TDG, 1), finished(false) {
  if (!this->TDG.has_specpre_trigger()) {
    panic("SpeculativePrecomputationTriggerInst with missing protobuf field.");
  }
}

void SpeculativePrecomputationTriggerInst::execute(LLVMTraceCPU *cpu) {
  cpu->getAcceleratorManager()
      ->getSpeculativePrecomputationManager()
      ->handleTrigger(this);
  this->markFinished();
}

void SpeculativePrecomputationTriggerInst::markFinished() {
  if (this->finished) {
    panic(
        "This SpeculativePrecomputationTriggerInst %u is already marked "
        "finished.",
        this->getId());
  }
  this->finished = true;
}

LLVMDynamicInst *parseSpeculativePrecomputationInst(
    LLVM::TDG::TDGInstruction &TDGInst) {
  switch (TDGInst.extra_case()) {
    case LLVM::TDG::TDGInstruction::ExtraCase::kSpecpreTrigger: {
      return new SpeculativePrecomputationTriggerInst(TDGInst);
    }
    default: { break; }
  }

  return nullptr;
}} // namespace gem5

