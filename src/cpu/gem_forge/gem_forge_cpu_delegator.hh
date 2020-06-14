
#ifndef __GEM_FORGE_CPU_DELEGATOR_HH__
#define __GEM_FORGE_CPU_DELEGATOR_HH__

#include "gem_forge_idea_inorder_cpu.hh"
#include "gem_forge_lsq_callback.hh"

#include "cpu/base.hh"
#include "params/BaseCPU.hh"

/**
 * Originally, these accelerators are implemented assuming a LLVMTraceCPU.
 * However, we may want to integrate them with Gem5's other execution-driven CPU
 * models for more realistic simulation. In order to avoid intrusive change to
 * existing CPU code, we add this intermediate layer -- CPUDelegator.
 *
 * This implementes the delegator interface.
 */

class GemForgeCPUDelegator {
public:
  enum CPUTypeE {
    LLVM_TRACE,
    TIMING_SIMPLE,
    MINOR,
  };
  const CPUTypeE cpuType;
  GemForgeCPUDelegator(CPUTypeE _cpuType, BaseCPU *_baseCPU);
  virtual ~GemForgeCPUDelegator() {}

  unsigned int cacheLineSize() const {
    return this->baseCPU->system->cacheLineSize();
  }
  /** Reads this CPU's ID. */
  int cpuId() const { return this->baseCPU->cpuId(); }
  /** Reads this CPU's unique data requestor ID. */
  MasterID dataMasterId() const { return this->baseCPU->dataMasterId(); }

  /**
   * The accelerators are implemented as SimObject, not ClockedObject,
   * so we provide some timing and scheduling functionality in the delegator.
   */
  Cycles curCycle() const { return this->baseCPU->curCycle(); }
  Tick cyclesToTicks(Cycles c) const { return this->baseCPU->cyclesToTicks(c); }
  void schedule(Event *event, Cycles latency) {
    this->baseCPU->schedule(event, this->baseCPU->clockEdge(latency));
  }
  void deschedule(Event *event) { this->baseCPU->deschedule(event); }

  /**
   * Get the ThreadContext.
   * Currently only support single thread per cpu.
   */
  ThreadContext *getSingleThreadContext() {
    assert(this->baseCPU->numContexts() == 1 &&
           "Can not support SMT CPU right now.");
    return this->baseCPU->getContext(0);
  }

  BaseTLB *getDataTLB() { return this->baseCPU->params()->dtb; }

  /**
   * Read a zero-terminated string from the memory.
   */
  std::string readStringFromMem(Addr vaddr);

  /**
   * Read from memory.
   */
  void readFromMem(Addr vaddr, int size, uint8_t *data);

  /**
   * Write to memory.
   */
  void writeToMem(Addr vaddr, int size, const uint8_t *data);

  /**
   * Really not sure how this should be implemeted in normal cpu.
   */
  virtual const std::string &getTraceExtraFolder() const = 0;

  /**
   * Immediately translate a vaddr to paddr. Panic when not possible.
   * TODO: Move this the some Process delegator.
   */
  virtual bool translateVAddrOracle(Addr vaddr, Addr &paddr) = 0;

  /**
   * Send a packet through the cpu.
   * If the CPU has a store buffer, it should be searched.
   */
  virtual void sendRequest(PacketPtr pkt) = 0;

  BaseCPU *baseCPU;

  /**
   * We have three idea inorder cpu modeling.
   */
  std::unique_ptr<GemForgeIdeaInorderCPU> ideaInorderCPU;
  std::unique_ptr<GemForgeIdeaInorderCPU> ideaInorderCPUNoFUTiming;
  std::unique_ptr<GemForgeIdeaInorderCPU> ideaInorderCPUNoLDTiming;
};

#endif