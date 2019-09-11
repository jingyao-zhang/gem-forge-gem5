#include "llvm_trace_cpu_delegator.hh"

Addr LLVMTraceCPUDelegator::translateVAddrOracle(Addr vaddr) {
  return this->cpu->translateAndAllocatePhysMem(vaddr);
}