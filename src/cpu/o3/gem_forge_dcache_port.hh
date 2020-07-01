#ifndef __CPU_O3_GEM_FORGE_DCACHE_PORT_HH
#define __CPU_O3_GEM_FORGE_DCACHE_PORT_HH

#include "lsq.hh"

#include "cpu/gem_forge/gem_forge_packet_handler.hh"

/*********************************************************************
 * Implementation of the GemForgeDcachePort in the LSQ.
 ********************************************************************/

template <class Impl>
bool LSQ<Impl>::GemForgeDcachePort::recvTimingResp(PacketPtr pkt) {
  // Intercept the GemForgePackets.
  if (GemForgePacketHandler::isGemForgePacket(pkt)) {
    GemForgePacketHandler::handleGemForgePacketResponse(
        this->cpu->getCPUDelegator(), pkt);
    return true;
  }
  // Normally call base handler.
  return DcachePort::recvTimingResp(pkt);
}

#endif