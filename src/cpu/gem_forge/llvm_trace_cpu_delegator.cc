#include "llvm_trace_cpu_delegator.hh"

bool LLVMTraceCPUDelegator::translateVAddrOracle(Addr vaddr, Addr &paddr) {
  paddr = this->cpu->translateAndAllocatePhysMem(vaddr);
  return true;
}
