#include "lsq.hh"

#include "cpu/gem_forge/gem_forge_packet_handler.hh"
#include "minor_cpu_delegator.hh"

/*********************************************************************
 * Implementation of the GemForgeDcachePort in the LSQ.
 ********************************************************************/

namespace Minor {

bool LSQ::GemForgeDcachePort::sendTimingReqVirtual(PacketPtr pkt) {
  // Update the used port info.
  if (this->curCycle != cpu.curCycle()) {
    this->curCycle = cpu.curCycle();
    this->numUsedPorts = 0;
  }
  if (this->blocked || this->numUsedPorts == this->numPorts ||
      this->blockedQueue.size()) {
    /**
     * Three reasons:
     * 1. Blocked.
     * 2. Ports are all used in this cycle.
     * 3. Waiting for some undrained packets.
     */
    this->blockedQueue.push(pkt);
    assert(this->blockedQueue.size() < 1024 && "Too many blocked packets.");

    // If not the first reason, schedule the drain event.
    if (!this->blocked && !this->drainEvent.scheduled()) {
      cpu.schedule(this->drainEvent, cpu.clockEdge(Cycles(1)));
    }
    return true;
  }
  assert(!this->drainEvent.scheduled());
  this->blockedQueue.push(pkt);
  this->drain();
  return true;
}

bool LSQ::GemForgeDcachePort::recvTimingResp(PacketPtr pkt) {
  // Intercept the GemForgePackets.
  if (GemForgePacketHandler::isGemForgePacket(pkt)) {
    GemForgePacketHandler::handleGemForgePacketResponse(cpu.cpuDelegator.get(),
                                                        pkt);
    return true;
  }
  // Normally call base handler.
  return DcachePort::recvTimingResp(pkt);
}

void LSQ::GemForgeDcachePort::recvReqRetry() {
  assert(this->blocked && "recvReqRetry while not blocked.");
  assert(!this->blockedQueue.empty() &&
         "recvReqRetry with empty blockedQueue.");
  this->blocked = false;
  assert(!this->drainEvent.scheduled() &&
         "Drain event should have not been scheduled.");
  this->drain();
}

void LSQ::GemForgeDcachePort::drain() {
  assert(!this->blocked && "drain while blocked.");
  assert(!this->blockedQueue.empty() && "drain with empty blockedQueue.");
  // Update the used port info.
  if (this->curCycle != cpu.curCycle()) {
    this->curCycle = cpu.curCycle();
    this->numUsedPorts = 0;
  }
  // Drain the blocked queue.
  while (!this->blockedQueue.empty() && !this->blocked &&
         this->numUsedPorts < this->numPorts) {
    auto pkt = this->blockedQueue.front();
    auto succeed = DcachePort::sendTimingReqVirtual(pkt);
    if (succeed) {
      this->numUsedPorts++;
      if (GemForgePacketHandler::isGemForgePacket(pkt)) {
        GemForgePacketHandler::issueToMemory(cpu.cpuDelegator.get(), pkt);
      }
      this->blockedQueue.pop();
    } else {
      // Blocked.
      this->blocked = true;
    }
  }
  if (!this->blocked && !this->blockedQueue.empty()) {
    // We haven't finished.
    assert(!this->drainEvent.scheduled());
    cpu.schedule(this->drainEvent, cpu.clockEdge(Cycles(1)));
  }
}
} // namespace Minor