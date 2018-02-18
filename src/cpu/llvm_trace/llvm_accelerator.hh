#ifndef __CPU_LLVM_ACCELERATOR_HH
#define __CPU_LLVM_ACCELERATOR_HH

#include "base/types.hh"

class LLVMAcceleratorContext {};

class LLVMAccelerator {
 public:
  LLVMAccelerator();

  Cycles getOpLatency(LLVMAcceleratorContext* context);
};

#endif