#include "llvm_trace_cpu_delegator.hh"

unsigned int LLVMTraceCPUDelegator::cacheLineSize() const {
  return this->cpu->system->cacheLineSize();
}

Addr LLVMTraceCPUDelegator::translateVAddrOracle(Addr vaddr) {
  return this->cpu->translateAndAllocatePhysMem(vaddr);
}