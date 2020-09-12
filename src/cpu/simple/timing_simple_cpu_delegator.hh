#ifndef __TIMING_SIMPLE_CPU_DELEGATOR_HH__
#define __TIMING_SIMPLE_CPU_DELEGATOR_HH__

/**
 * This implements the delegator interface for the SimpleTimingCPU.
 */

#include "simple_cpu_delegator.hh"
#include "timing.hh"

class TimingSimpleCPUDelegator : public SimpleCPUDelegator {
public:
  TimingSimpleCPUDelegator(TimingSimpleCPU *_cpu);
  ~TimingSimpleCPUDelegator() override;

  void sendRequest(PacketPtr pkt) override;

};

#endif