#include "atomic_simple_cpu_delegator.hh"

#include "cpu/gem_forge/gem_forge_packet_handler.hh"

/**************************************************************************
 * AtomicSimpleCPUDelegator.
 *************************************************************************/

AtomicSimpleCPUDelegator::AtomicSimpleCPUDelegator(AtomicSimpleCPU *_cpu)
    : SimpleCPUDelegator(CPUTypeE::ATOMIC_SIMPLE, _cpu) {}
AtomicSimpleCPUDelegator::~AtomicSimpleCPUDelegator() = default;

void AtomicSimpleCPUDelegator::sendRequest(PacketPtr pkt) {
  // In atomic mode we can directly send the packet.
  auto cpu = dynamic_cast<AtomicSimpleCPU *>(this->baseCPU);
  assert(cpu && "Must be AtomicSimpleCPU");
  assert(GemForgePacketHandler::isGemForgePacket(pkt));
  GemForgePacketHandler::issueToMemory(this, pkt);
  auto latency = cpu->sendPacket(cpu->dcachePort, pkt);
  // Just stop the compiler complaining about unused variable.
  (void)latency;
  // For simplicity now we immediately send back response.
  GemForgePacketHandler::handleGemForgePacketResponse(this, pkt);
}
