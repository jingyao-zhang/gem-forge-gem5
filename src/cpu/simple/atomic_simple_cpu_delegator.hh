#ifndef __ATOMIC_SIMPLE_CPU_DELEGATOR_HH__
#define __ATOMIC_SIMPLE_CPU_DELEGATOR_HH__

/**
 * This implements the delegator interface for the SimpleAtomicCPU.
 */

#include "atomic.hh"
#include "simple_cpu_delegator.hh"

class AtomicSimpleCPUDelegator : public SimpleCPUDelegator {
public:
  AtomicSimpleCPUDelegator(AtomicSimpleCPU *_cpu);
  ~AtomicSimpleCPUDelegator() override;

  void sendRequest(PacketPtr pkt) override;
};

#endif