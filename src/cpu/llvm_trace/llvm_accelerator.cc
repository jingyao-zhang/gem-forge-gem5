#include "base/misc.hh"
#include "cpu/llvm_trace/llvm_accelerator.hh"

LLVMAccelerator::LLVMAccelerator() {}

Cycles LLVMAccelerator::getOpLatency(LLVMAcceleratorContext* context) {
  if (context == nullptr) {
    panic("Null context.\n");
  }
  return Cycles(context->latency);
}

LLVMAcceleratorContext::LLVMAcceleratorContext(unsigned long _latency)
    : latency(_latency) {}

LLVMAcceleratorContext* LLVMAcceleratorContext::parseContext(
    const std::string& str) {
  unsigned long latency = stoul(str);
  return new LLVMAcceleratorContext(latency);
}