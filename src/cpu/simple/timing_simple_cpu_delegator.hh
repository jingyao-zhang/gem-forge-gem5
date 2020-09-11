#ifndef __TIMING_SIMPLE_CPU_DELEGATOR_HH__
#define __TIMING_SIMPLE_CPU_DELEGATOR_HH__

/**
 * This implements the delegator interface for the SimpleTimingCPU.
 */

#include "cpu/gem_forge/gem_forge_cpu_delegator.hh"
#include "timing.hh"

class TimingSimpleCPUDelegator : public GemForgeCPUDelegator {
public:
  TimingSimpleCPUDelegator(TimingSimpleCPU *_cpu);
  ~TimingSimpleCPUDelegator() override;

  const std::string &getTraceExtraFolder() const override;

  bool translateVAddrOracle(Addr vaddr, Addr &paddr) override;

  void sendRequest(PacketPtr pkt) override;

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