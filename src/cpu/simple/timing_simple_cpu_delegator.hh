#ifndef __TIMING_SIMPLE_CPU_DELEGATOR_HH__
#define __TIMING_SIMPLE_CPU_DELEGATOR_HH__

/**
 * This implements the delegator interface for the SimpleTimingCPU.
 */

#include "cpu/gem_forge/gem_forge_cpu_delegator.hh"
#include "timing.hh"

class TimingSimpleCPUDelegator : public GemForgeCPUDelegator {
public:
  TimingSimpleCPUDelegator(TimingSimpleCPU *_cpu)
      : GemForgeCPUDelegator(CPUTypeE::TIMING_SIMPLE, _cpu), cpu(_cpu) {}

  const std::string &getTraceExtraFolder() const override {
    panic("Not implemented yet.");
  }

  Addr translateVAddrOracle(Addr vaddr) override {
    panic("Not implemented yet.");
  }

  void sendRequest(PacketPtr pkt) override { panic("Not implemented yet."); }

  TimingSimpleCPU *cpu;
};

#endif