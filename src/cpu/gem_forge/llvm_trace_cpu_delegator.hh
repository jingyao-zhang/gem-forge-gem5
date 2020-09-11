
#ifndef __LLVM_TRACE_CPU_DELEGATOR_HH__
#define __LLVM_TRACE_CPU_DELEGATOR_HH__
/**
 * This implementes the delegator for the LLVMTraceCPU.
 */

#include "cpu/gem_forge/gem_forge_cpu_delegator.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"

class LLVMTraceCPUDelegator : public GemForgeCPUDelegator {
public:
  LLVMTraceCPUDelegator(LLVMTraceCPU *_cpu)
      : GemForgeCPUDelegator(CPUTypeE::LLVM_TRACE, _cpu), cpu(_cpu) {}

  const std::string &getTraceExtraFolder() const override {
    return this->cpu->getTraceExtraFolder();
  }

  bool translateVAddrOracle(Addr vaddr, Addr &paddr) override;
  void sendRequest(PacketPtr pkt) override { this->cpu->sendRequest(pkt); }

  LLVMTraceCPU *cpu;

protected:
  InstSeqNum getInstSeqNum() const override;
  void setInstSeqNum(InstSeqNum seqNum) override;
};

#endif