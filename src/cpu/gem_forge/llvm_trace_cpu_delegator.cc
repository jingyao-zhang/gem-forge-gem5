#include "llvm_trace_cpu_delegator.hh"

namespace gem5 {

bool LLVMTraceCPUDelegator::translateVAddrOracle(Addr vaddr, Addr &paddr) {
  paddr = this->cpu->translateAndAllocatePhysMem(vaddr);
  return true;
}

InstSeqNum LLVMTraceCPUDelegator::getInstSeqNum() const {
  return LLVMDynamicInst::getGlobalSeqNum();
}

void LLVMTraceCPUDelegator::setInstSeqNum(InstSeqNum seqNum) {
  LLVMDynamicInst::setGlobalSeqNum(seqNum);
}

void LLVMTraceCPUDelegator::recordStatsForFakeExecutedInst(
    const StaticInstPtr &inst) {
  // So far we do nothing.
}} // namespace gem5

