#include "timing_simple_cpu_delegator.hh"

/**************************************************************************
 * TimingSimpleCPUDelegator.
 *************************************************************************/

TimingSimpleCPUDelegator::TimingSimpleCPUDelegator(TimingSimpleCPU *_cpu)
    : SimpleCPUDelegator(CPUTypeE::TIMING_SIMPLE, _cpu) {}
TimingSimpleCPUDelegator::~TimingSimpleCPUDelegator() = default;

void TimingSimpleCPUDelegator::sendRequest(PacketPtr pkt) {
  // The CPU's port should already be a GemForgeDcachePort.
  auto cpu = dynamic_cast<TimingSimpleCPU *>(this->baseCPU);
  assert(cpu && "Must be TimingSimpleCPU");
  assert(dynamic_cast<TimingSimpleCPU::GemForgeDcachePort *>(
             cpu->dcachePort.get()) &&
         "GemForgeCPUDelegator::sendRequest called when the DcachePort is not "
         "a GemForge port.");
  auto succeed = cpu->dcachePort->sendTimingReqVirtual(pkt);
  assert(succeed && "GemForgePort should always succeed on sending TimingReq.");
}