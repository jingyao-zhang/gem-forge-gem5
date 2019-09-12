
#ifndef __GEM_FORGE_CPU_DELEGATOR_HH__
#define __GEM_FORGE_CPU_DELEGATOR_HH__

#include "cpu/base.hh"

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
  GemForgeCPUDelegator(BaseCPU *_baseCPU) : baseCPU(_baseCPU) {}
  virtual ~GemForgeCPUDelegator() {}

  unsigned int cacheLineSize() const {
    return this->baseCPU->system->cacheLineSize();
  }
  /** Reads this CPU's ID. */
  int cpuId() const { return this->baseCPU->cpuId(); }
  /** Reads this CPU's unique data requestor ID. */
  MasterID dataMasterId() const { return this->baseCPU->dataMasterId(); }

  /**
   * Really not sure how this should be implemeted in normal cpu.
   */
  virtual const std::string &getTraceExtraFolder() const = 0;

  /**
   * The accelerators are implemented as SimObject, not ClockedObject,
   * so we provide some timing and scheduling functionality in the delegator.
   */
  Cycles curCycle() const { return this->baseCPU->curCycle(); }
  Tick cyclesToTicks(Cycles c) const { return this->baseCPU->cyclesToTicks(c); }
  void schedule(Event *event, Cycles latency) {
    this->baseCPU->schedule(event, this->baseCPU->clockEdge(latency));
  }

  /**
   * Immediately translate a vaddr to paddr. Panic when not possible.
   * TODO: Move this the some Process delegator.
   */
  virtual Addr translateVAddrOracle(Addr vaddr) = 0;

  /**
   * Send a packet through the cpu.
   */
  virtual void sendRequest(PacketPtr pkt) = 0;

  BaseCPU *baseCPU;
};

#endif