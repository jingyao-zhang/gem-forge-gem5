#ifndef __O3_CPU_DELEGATOR_HH__
#define __O3_CPU_DELEGATOR_HH__

#include "cpu/gem_forge/gem_forge_cpu_delegator.hh"

/******************************************************************
 * This provides the interface between O3CPU and GemForge.
 *
 * GemForge accelerators assume an out-of-order execution that:
 * 1. In-order dispatch, mem-ref instructions will be inserted into
 *    LSQ (e.g. first user instruction of a stream element).
 * 2. Out-of-order issue, when dependence is ready.
 * 3. In-order commit.
 *
 * O3CPU is an out-of-order CPU that fits well in this model:
 * 1. In-order rename -- we take this as our dispatch stage, instead
 *    of conventional issue stage, as reanmed instructions are pushed
 *    into IQ by issue stage, and ROB by commit stage (wierd design),
 *    and there is no break point once in commit stage.
 * 2.
 *******************************************************************/

template <class CPUImpl>
class DefaultO3CPUDelegator : public GemForgeCPUDelegator {
public:
  using O3CPU = typename CPUImpl::O3CPU;
  using DynInstPtr = typename CPUImpl::DynInstPtr;

  DefaultO3CPUDelegator(O3CPU *_cpu);
  ~DefaultO3CPUDelegator() override;

  const std::string &getTraceExtraFolder() const override;
  bool translateVAddrOracle(Addr vaddr, Addr &paddr) override;
  void sendRequest(PacketPtr pkt) override;

  /***************************************************************
   * Interface to the CPU.
   ***************************************************************/

  bool canDispatch(DynInstPtr &dynInstPtr);
  void dispatch(DynInstPtr &dynInstPtr);
  bool canExecute(DynInstPtr &dynInstPtr);
  void execute(DynInstPtr &dynInstPtr);
  bool canCommit(const DynInstPtr &dynInstPtr);
  void commit(const DynInstPtr &dynInstPtr);

  /**
   * Squash all instructions younger than this SeqNum.
   */
  void squash(InstSeqNum squashSeqNum);

private:
  class Impl;
  std::unique_ptr<Impl> pimpl;
};

#endif