#ifndef __ATOMIC_SIMPLE_CPU_DELEGATOR_HH__
#define __ATOMIC_SIMPLE_CPU_DELEGATOR_HH__

/**
 * This implements the delegator interface for the SimpleAtomicCPU.
 */

#include "atomic.hh"
#include "cpu/gem_forge/gem_forge_cpu_delegator.hh"

class AtomicSimpleCPUDelegator : public GemForgeCPUDelegator {
public:
  AtomicSimpleCPUDelegator(AtomicSimpleCPU *_cpu);
  ~AtomicSimpleCPUDelegator() override;

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
};

#endif