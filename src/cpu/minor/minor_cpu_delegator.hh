#ifndef __MINOR_CPU_DELEGATOR_HH__
#define __MINOR_CPU_DELEGATOR_HH__

#include "cpu.hh"
#include "cpu/gem_forge/gem_forge_cpu_delegator.hh"
#include "dyn_inst.hh"

class MinorCPUDelegator : public GemForgeCPUDelegator {
public:
  MinorCPUDelegator(MinorCPU *_cpu);
  ~MinorCPUDelegator() override;

  const std::string &getTraceExtraFolder() const override;
  Addr translateVAddrOracle(Addr vaddr) override;
  void sendRequest(PacketPtr pkt) override;

  /**
   * Interface to the CPU.
   * We can use the execSeqNum as the sequence number.
   */
  bool canDispatch(Minor::MinorDynInstPtr &dynInstPtr);
  void dispatch(Minor::MinorDynInstPtr &dynInstPtr);
  bool canExecute(Minor::MinorDynInstPtr &dynInstPtr);
  void execute(Minor::MinorDynInstPtr &dynInstPtr, ExecContext &xc);
  void commit(Minor::MinorDynInstPtr &dynInstPtr);

  /**
   * Control misspeculation happened.
   * * The branch caused the stream change must be committed before
   * * this, otherwise it will be considered as a misspeculated inst
   * * and rewinded. Luckily this is the case in MinorCPU so far.
   */
  void streamChange(InstSeqNum newStreamSeqNum);

private:
  class Impl;
  std::unique_ptr<Impl> pimpl;
};

#endif
