#ifndef __GEM_FORGE_DCACHE_PORT_HH__
#define __GEM_FORGE_DCACHE_PORT_HH__

#include "cpu/base.hh"
#include "mem/port.hh"

namespace GemForge {
/**
 * GemForgeDcachePort uses a queue in the CPUPort to buffer the requests
 * and never returns false on sendTimingReq. It drains the queue on retry
 * at 2 packets per cycle. The response is handled one packet per cycle,
 * which is the original design.
 */
class GemForgeDcachePortImpl {
public:
  GemForgeDcachePortImpl(BaseCPU *_cpu, MasterPort *_port)
      : cpu(_cpu), port(_port), blocked(false), curCycle(0), numUsedPorts(0),
        drainEvent([this]() -> void { this->drain(); }, name()) {}

  /**
   * API to the port.
   */
  bool sendTimingReqVirtual(PacketPtr pkt, bool isCore);
  void recvReqRetry();

protected:
  BaseCPU *cpu;
  MasterPort *port;

  const int numPorts = 2;
  std::list<std::pair<PacketPtr, bool>> blockedQueue;
  bool blocked;
  /**
   * If we have seen a lock RMWRead, the port should be blocked
   * until a RMWWrite comes in.
   */
  std::set<Addr> lockedRMWLinePAddr;
  Cycles curCycle;
  int numUsedPorts;

  EventFunctionWrapper drainEvent;

  void drain();

  // Steal the port's name.
  const std::string name() const { return this->port->name(); }
};
} // namespace GemForge

#endif