
#ifndef __GEM_FORGE_CPU_DELEGATOR_HH__
#define __GEM_FORGE_CPU_DELEGATOR_HH__

#include "cpu/base.hh"

/**
 * Originally, these accelerators are implemented assuming a LLVMTraceCPU.
 * However, we may want to integrate them with Gem5's other execution-driven CPU
 * model for more realistic simulation. In order to avoid intrusive change to
 * existing CPU code, we add this intermediate layer -- CPUDelegator.
 *
 * This implementes the delegator interface.
 */

class GemForgeCPUDelegator {
public:
  virtual ~GemForgeCPUDelegator() {}

  virtual unsigned int cacheLineSize() const = 0;

  /**
   * The accelerators are implemented as SimObject, not ClockedObject,
   * so we provide some timing and scheduling functionality in the delegator.
   */
  virtual Cycles curCycle() const = 0;
  virtual Tick cyclesToTicks(Cycles c) const = 0;

  /**
   * Immediately translate a vaddr to paddr. Panic when not possible.
   * TODO: Move this the some Process delegator.
   */
  virtual Addr translateVAddrOracle(Addr vaddr) = 0;
};

#endif