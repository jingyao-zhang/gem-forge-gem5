#ifndef __CPU_LLVM_STAGE_SIGNAL__
#define __CPU_LLVM_STAGE_SIGNAL__

struct LLVMStageSignal {
  bool stall;
  LLVMStageSignal() : stall(false) {}
};

#endif