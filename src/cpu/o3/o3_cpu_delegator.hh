#ifndef __O3_CPU_DELEGATOR_HH__
#define __O3_CPU_DELEGATOR_HH__

#include "cpu/gem_forge/gem_forge_cpu_delegator.hh"
#include "cpu/o3/gem_forge_load_request.hh"

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
 * 2. In-order dispatch -- Here the mem-ref instructions are inserted
 *    into the LSQ. And the instruction queue will monity ready insts
 *    and schedule them.
 * 3. Out-of-order execute/writeback -- Once scheduled, the instruction
 *    will be executed. For mem-ref instructions, this means the
 *    address is computed and start to translate/access memory.
 * 4. In-order commit.
 *
 * Similar to MinorCPU, we use a PreLSQ to track instructions from
 * dispatched to have LSQRequest in the LSQ. In short, GemForgeLoad
 * instructions with GemForgeLoadCallback will be:
 * 1. At dispatch, insert into PreLSQ.
 * 2. When the address is ready, mark canExecute.
 * 3. In LSQ::executeLoad, when the LSQ can track the address, we remove
 *    it from the PreLSQ and allocate GemForgeLoadRequest and insert
 *    into the LSQ.
 * 4. For each InLSQ GemForgeLoadReq, we have a special event to check if
 *    GemForge say the value is ready.
 * 5. If found a misspeculation, we flush everything.
 *
 * GemForgeStore is a little different: the request is still handled by
 * the core, and we only get the address and value from GemForge. Therefore,
 * it is very similar to normal stores.
 * 1. At dispatch, insert into PreLSQ.
 * 2. When the address AND value are ready, mark canExecute.
 * 3. In LSQ::executeStore, we call pushRequest to inform the core about
 *    the store address and value. BUT we do not remove it from PreLSQ, as
 *    the core may defer the store request (e.g. due to TLB miss), and we
 *    do not want to lose the callback in this case.
 * 4. If found a misspeculation, the core will flush everything and restart
 *    again. GemForge don't have to worry about it any more.
 *
 * GemForgeAtomic is a hybrid case: it is treated as a non-speculative
 * store by the core, yet the request is sent out by the core SE (similar
 * to GemForgeLoad).
 *
 *******************************************************************/

template <class CPUImpl>
class DefaultO3CPUDelegator : public GemForgeCPUDelegator {
public:
  using O3CPU = typename CPUImpl::O3CPU;
  using LSQUnit = typename CPUImpl::CPUPol::LSQUnit;
  using DynInstPtr = typename CPUImpl::DynInstPtr;
  using GFLoadReq = GemForgeLoadRequest<CPUImpl>;

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
  bool canWriteback(const DynInstPtr &dynInstPtr);
  void writeback(const DynInstPtr &dynInstPtr);
  bool canCommit(const DynInstPtr &dynInstPtr);
  void commit(const DynInstPtr &dynInstPtr);

  bool shouldCountInPipeline(const DynInstPtr &dynInstPtr);

  /**
   * Squash all instructions younger than this SeqNum.
   */
  void squash(InstSeqNum squashSeqNum);

  /**
   * CPU commit a store to this address. We need to:
   * 1. Notify GemForge.
   * 2. Invalid any aliased load in PreLSQ.
   */
  void storeTo(InstSeqNum seqNum, Addr vaddr, int size);

  /**
   * CPU LSQ found a RAWMisspuclation at SeqNum. We need to:
   * 1. Call RAWMisspeculation on every callback in LSQ >= SeqNum.
   * 2. Call RAWMisspeculation on every callback in PreLSQ.
   *
   * Later these instruction will be squshed by the CPU.
   *
   * This is overconservative, as some data may be dependent
   * on the misspeculated data. But we take it seriously.
   */
  void foundRAWMisspeculationInLSQ(InstSeqNum squashSeqNum, Addr vaddr,
                                   int size);

  /**
   * This is the real initiateAcc for GemForgeLoad/Atomic and GemForgeStore.
   */
  Fault initiateGemForgeLoadOrAtomic(const DynInstPtr &dynInstPtr);
  Fault initiateGemForgeStore(const DynInstPtr &dynInstPtr);

  /**
   * When the core execute a GemForgeLoad, it calls this
   * to get a special GemForgeLoadRequest.
   * @return nullptr if this is not a GemForgeLoad.
   */
  GFLoadReq *allocateGemForgeLoadRequest(LSQUnit *lsq,
                                         const DynInstPtr &dynInstPtr);

  /**
   * The GemForgeLoadRequest is discarded by the LSQ, we
   * should add it back to PreLSQ.
   */
  void discardGemForgeLoad(const DynInstPtr &dynInstPtr,
                           GemForgeLSQCallbackPtr callback);

private:
  class Impl;
  std::unique_ptr<Impl> pimpl;

protected:
  InstSeqNum getInstSeqNum() const override;
  void setInstSeqNum(InstSeqNum seqNum) override;
};

#endif
