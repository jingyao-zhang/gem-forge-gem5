#ifndef __MINOR_CPU_DELEGATOR_HH__
#define __MINOR_CPU_DELEGATOR_HH__

#include "cpu/gem_forge/gem_forge_cpu_delegator.hh"
#include "cpu/minor/cpu.hh"
#include "cpu/minor/dyn_inst.hh"

/*************************************************************************
 * This provides the interface between MinorCPU and GemForge.
 *
 * GemForge accelerators assume an out-of-order execution that:
 * 1. In-order dispatch, mem-ref instructions will be inserted int LSQ (e.g.
 *    first user instruction of a stream element).
 * 2. Out-of-order issue, when dependence is ready.
 * 3. In-order commit.
 *
 * MinorCPU is an in-order CPU that:
 * 1. In-order issue to FU, when all dependence is ready.
 * 2. Mem-ref instructions are inserted into LSQ when coming out of the FU
 *    (vaddr ready, earlyMemIssue).
 * 3. In-order commit.
 *
 * The major differences are:
 * 1. In MinorCPU, FU is strictly before the LSQ, i.e. instructions in the
 *    LSQ have no access to the FU. This means that a dummy StreamLoad is
 *    required to travel through the LSQ and maintain the memory order.
 * 2. The LSQ entry is not reserved at issue time, but after completing
 *    the FU.
 *
 * To solve this, we add a PreLSQ to hold instructions that require
 * monitoring memory aliasing information before inserted into the LSQ,
 * e.g. StreamLoad, as these instructions represent an memory access way before
 * dispatching. This PreLSQ can be unordered.
 *************************************************************************/

class MinorCPUDelegator : public GemForgeCPUDelegator {
public:
  MinorCPUDelegator(MinorCPU *_cpu);
  ~MinorCPUDelegator() override;

  const std::string &getTraceExtraFolder() const override;
  bool translateVAddrOracle(Addr vaddr, Addr &paddr) override;
  void sendRequest(PacketPtr pkt) override;

  /**
   * Interface to the CPU.
   * We can use the execSeqNum as the sequence number.
   */
  void startup();

  /**
   * Whether this instruction should be count in Fetch2 and Decode.
   * Note that these are macroinstructions.
   */
  bool shouldCountInFrontend(Minor::MinorDynInstPtr &dynInstPtr);

  bool canDispatch(Minor::MinorDynInstPtr &dynInstPtr);
  void dispatch(Minor::MinorDynInstPtr &dynInstPtr);

  /**
   * Check if the addr/size of this mem-ref instruction is ready,
   * (if it has extra LQ callbacks).
   * This is required to inform the cpu to block the insertion into LSQ, which
   * requires the addr/size of the access.
   * @return true if there is no LQCallback in PreLSQ, or the callbacks'
   * addr/size are ready.
   */
  bool isAddrSizeReady(Minor::MinorDynInstPtr &dynInstPtr);

  /**
   * Insert the GemForgeLQCallback into the LSQ.
   * This requires that the addr/size of this callback is ready.
   *
   * Returns the Fault if we ever encounter a translation fault.
   */
  Fault insertLSQ(Minor::MinorDynInstPtr &dynInstPtr);

  /**
   * ! This is not used right now.
   * ! The following problem is fixed by making StreamReady a memory barrier.
   * Get any inst that has to commit before MemRef inst can be early issued.
   * This is to solve a live-lock problem:
   * A StreamLoad may reached the head of the FU and need to be early issued
   * before the stream is configured. The problem is:
   * StreamNotConfigured -> StreamElementAddrNotReady -> StreamLoadCannotIssue
   *           |                                             |
   *           ------------------- <- ------------------------
   * The implementation of commit logic is that if EarlyIssue failed, MinorCPU
   * won't try to commit other instructions.
   */
  InstSeqNum getEarlyIssueMustWaitSeqNum(Minor::MinorDynInstPtr &dynInstPtr);

  bool canExecute(Minor::MinorDynInstPtr &dynInstPtr);
  void execute(Minor::MinorDynInstPtr &dynInstPtr, ExecContext &xc);
  bool canCommit(Minor::MinorDynInstPtr &dynInstPtr);
  void commit(Minor::MinorDynInstPtr &dynInstPtr);

  /**
   * Control misspeculation happened.
   * * The branch caused the stream change must be committed before
   * * this, otherwise it will be considered as a misspeculated inst
   * * and rewinded, as it has the old streamSeqNum.
   */
  void streamChange(InstSeqNum newStreamSeqNum);

  /**
   * CPU stores to a place. Notify GemForge to check any memory misspeculation.
   */
  void storeTo(Addr vaddr, int size);

private:
  class Impl;
  std::unique_ptr<Impl> pimpl;

  /**
   * This function can't be implemented in Impl as it requires access to
   * the LSQ private data.
   */
  void drainPendingPackets();
};

#endif
