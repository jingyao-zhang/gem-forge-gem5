
#ifndef __GEM_FORGE_CPU_DELEGATOR_HH__
#define __GEM_FORGE_CPU_DELEGATOR_HH__
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
};

#endif