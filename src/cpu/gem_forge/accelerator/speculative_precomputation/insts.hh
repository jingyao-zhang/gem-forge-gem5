
#ifndef __CPU_TDG_ACCELERATOR_SPECULATIVE_PRECOMPUTATION_INST_H__
#define __CPU_TDG_ACCELERATOR_SPECULATIVE_PRECOMPUTATION_INST_H__

#include "cpu/gem_forge/llvm_insts.hh"

namespace gem5 {

class SpeculativePrecomputationTriggerInst : public LLVMDynamicInst {
 public:
  SpeculativePrecomputationTriggerInst(const LLVM::TDG::TDGInstruction &_TDG);

  void execute(LLVMTraceCPU *cpu) override;
  bool isCompleted() const override { return this->finished; }

  /**
   * Interface for AbstractDataFlowAccelerator.
   */
  void markFinished();

 private:
  bool finished;
};

/**
 * Helper function used by DynamicInstructionStream to parse the TDGInstruction.
 * Returns nullptr if this is not speculative precomputation instruction.
 */
LLVMDynamicInst *parseSpeculativePrecomputationInst(
    LLVM::TDG::TDGInstruction &TDGInst);

} // namespace gem5

#endif