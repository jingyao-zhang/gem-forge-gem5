#ifndef __SIMPLE_CPU_DELEGATOR_HH__
#define __SIMPLE_CPU_DELEGATOR_HH__

/**
 * This implements the delegator interface for the SimpleTimingCPU.
 */

#include "cpu/gem_forge/gem_forge_cpu_delegator.hh"
#include "cpu/simple/base.hh"

class SimpleCPUDelegator : public GemForgeCPUDelegator {
public:
  SimpleCPUDelegator(CPUTypeE _cpuType, BaseSimpleCPU *_cpu);
  ~SimpleCPUDelegator() override;

  const std::string &getTraceExtraFolder() const override;

  bool translateVAddrOracle(Addr vaddr, Addr &paddr) override;

  /**
   * Interface to the CPU.
   */
  bool canDispatch(StaticInstPtr staticInst, ExecContext &xc);
  void dispatch(StaticInstPtr staticInst, ExecContext &xc);
  bool canExecute(StaticInstPtr staticInst, ExecContext &xc);
  void execute(StaticInstPtr staticInst, ExecContext &xc);
  bool canCommit(StaticInstPtr staticInst, ExecContext &xc);
  void commit(StaticInstPtr staticInst, ExecContext &xc);
  void storeTo(Addr vaddr, int size);

private:
  class Impl;
  std::unique_ptr<Impl> pimpl;

protected:
  InstSeqNum getInstSeqNum() const override;
  void setInstSeqNum(InstSeqNum seqNum) override;
};

#endif