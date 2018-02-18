#include "cpu/llvm_trace/llvm_accelerator.hh"

LLVMAccelerator::LLVMAccelerator() {}

Cycles LLVMAccelerator::getOpLatency(LLVMAcceleratorContext* context) {
  return Cycles(1);
}