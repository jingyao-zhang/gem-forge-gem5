#ifndef __CPU_TDG_ACCELERATOR_STREAM_INST_H__
#define __CPU_TDG_ACCELERATOR_STREAM_INST_H__

#include "cpu/llvm_trace/llvm_insts.hh"

class StreamConfigInst : public LLVMDynamicInst {
public:
  StreamConfigInst(const LLVM::TDG::TDGInstruction &_TDG);
  void execute(LLVMTraceCPU *cpu) override;
  bool isCompleted() const override { return this->finished; }

  /**
   * Interface for stream engine.
   */
  void markFinished();

private:
  bool finished;
};

class StreamStepInst : public LLVMDynamicInst {
public:
  StreamStepInst(const LLVM::TDG::TDGInstruction &_TDG);
  void execute(LLVMTraceCPU *cpu) override;
  bool isCompleted() const override { return this->finished; }

  /**
   * Interface for stream engine.
   */
  void markFinished();

private:
  bool finished;
};

class StreamStoreInst : public LLVMDynamicInst {
public:
  StreamStoreInst(const LLVM::TDG::TDGInstruction &_TDG);
  void execute(LLVMTraceCPU *cpu) override;
  bool isCompleted() const override { return this->finished; }

  /**
   * Interface for stream engine.
   */
  void markFinished();

private:
  bool finished;
};

/**
 * Helper function used by DynamicInstructionStream to parse the TDGInstruction.
 * Returns nullptr if this is not stream instruction.
 */

LLVMDynamicInst *parseStreamInst(LLVM::TDG::TDGInstruction &TDGInst);

#endif