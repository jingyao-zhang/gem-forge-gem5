
#ifndef __LLVM_TRACE_CPU_DELEGATOR_HH__
#define __LLVM_TRACE_CPU_DELEGATOR_HH__
/**
 * This implementes the delegator for the LLVMTraceCPU.
 */

#include "cpu/gem_forge/accelerator/gem_forge_cpu_delegator.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"

class LLVMTraceCPUDelegator : public GemForgeCPUDelegator {
public:
  LLVMTraceCPUDelegator(LLVMTraceCPU *_cpu) : cpu(_cpu) {}

  LLVMTraceCPU *cpu;
};

#endif