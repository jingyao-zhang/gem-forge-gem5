#include "llvm_trace_cpu_delegator.hh"

unsigned int LLVMTraceCPUDelegator::cacheLineSize() const {
  return this->cpu->system->cacheLineSize();
}
