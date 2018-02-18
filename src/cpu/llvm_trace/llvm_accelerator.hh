#ifndef __CPU_LLVM_ACCELERATOR_HH
#define __CPU_LLVM_ACCELERATOR_HH

#include <string>

#include "base/types.hh"

class LLVMAccelerator;

class LLVMAcceleratorContext {
 public:
  LLVMAcceleratorContext(unsigned long _latency);

  // Return a newed context.
  // Caller is responsible to free it if necessary.
  static LLVMAcceleratorContext* parseContext(const std::string& str);

 private:
  friend class LLVMAccelerator;
  const unsigned long latency;
};

class LLVMAccelerator {
 public:
  LLVMAccelerator();

  Cycles getOpLatency(LLVMAcceleratorContext* context);
};

#endif