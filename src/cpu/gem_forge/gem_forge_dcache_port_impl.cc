#include "gem_forge_dcache_port_impl.hh"

#include "cpu/gem_forge/gem_forge_packet_handler.hh"

#include "debug/GemForgeDcachePort.hh"

/*********************************************************************
 * Implementation of the GemForgeDcachePort in the LSQ.
 ********************************************************************/

namespace GemForge {

bool GemForgeDcachePortImpl::sendTimingReqVirtual(PacketPtr pkt, bool isCore) {
  // Update the used port info.
  if (this->curCycle != cpu->curCycle()) {
    this->curCycle = cpu->curCycle();
    this->numUsedPorts = 0;
  }

  DPRINTF(GemForgeDcachePort, "Receive isCore %d, %s.\n", isCore, pkt->print());
  assert(this->blockedQueue.size() < 2048 && "Too many blocked packets.");

  /**
   * Priortize the core request, as it involves some deadlocks.
   * One case:
   * CoreRMWRead    0x10
   * StreamRead     0x10 (blocked by RMWRead lock)
   * CoreStore      0x80 (blocked by head of queue)
   * CoreRMWWrite   0x10 (not issued by StoreBuffer, blocked by previous
   * CoreStore)
   *
   * We have to make sure core requests are served first.
   */

  if (isCore) {
    auto iter = this->blockedQueue.begin();
    auto end = this->blockedQueue.end();
    while (iter != end) {
      if (!iter->second) {
        // Found first non-core request.
        break;
      }
      DPRINTF(GemForgeDcachePort, "Skip core req %s.\n", iter->first->print());
      ++iter;
    }
    // Insert.
    this->blockedQueue.emplace(iter, pkt, isCore);
  } else {
    /**
     * Normally we just insert the request at the end.
     * But some requests need to bypass the queue to avoid deadlock.
     * For example a floated stream is rewinded, and port is already blocked by
     * inflying requests. However, now the LLC SE may not sending responses,
     * and MLC SE has no idea this stream is ended.
     * Therefore, we bypass the queue for StreamConfig/End request, to ensure
     * that streams are configured before ended.
     */
    if (pkt->cmd == MemCmd::Command::StreamConfigReq ||
        pkt->cmd == MemCmd::Command::StreamEndReq) {
      DPRINTF(GemForgeDcachePort,
              "Bypass the queue for StreamConfig/EndReq.\n");
      bool succeed = this->port->sendTimingReq(pkt);
      if (!succeed) {
        panic("StreamConfig/EndReq should always succeed.\n");
      }
      return true;
    }
    // Simply insert at the end.
    DPRINTF(GemForgeDcachePort, "Insert at the end.\n");
    this->blockedQueue.emplace_back(pkt, isCore);
  }

  /**
   * Special sanity check for RMWWrite.
   */
  if (pkt->req->isLockedRMW() && pkt->isWrite()) {
    assert(isCore && "RMWWrite should be from core.");
    auto paddr = pkt->req->getPaddr();
    auto paddrLine = paddr & (~(cpu->system->cacheLineSize() - 1));
    // In minor CPU, the RMWRead should already be seen by the cache.
    if (!this->lockedRMWLinePAddr.count(paddrLine)) {
      fatal("%lu: %s: Not locked when RMWWrite comes %#x.\n", curTick(), name(),
            paddrLine);
    }
  }

  if (this->blocked || this->numUsedPorts == this->numPorts) {
    /**
     * Two reasons:
     * 1. Blocked.
     * 2. Ports are all used in this cycle.
     */
    if (Debug::GemForgeDcachePort) {
      if (this->blocked) {
        DPRINTF(GemForgeDcachePort, "Blocked.\n");
      } else if (this->numUsedPorts == this->numPorts) {
        DPRINTF(GemForgeDcachePort, "Used all %d ports.\n", this->numPorts);
      }
    }
    // If not the first reason, schedule the drain event.
    if (!this->blocked && !this->drainEvent.scheduled()) {
      cpu->schedule(this->drainEvent, cpu->clockEdge(Cycles(1)));
    }
  } else {
    // We can drain immediately.
    if (this->drainEvent.scheduled()) {
      cpu->deschedule(this->drainEvent);
    }
    this->drain();
  }
  return true;
}

void GemForgeDcachePortImpl::recvReqRetry() {
  assert(this->blocked && "recvReqRetry while not blocked.");
  assert(!this->blockedQueue.empty() &&
         "recvReqRetry with empty blockedQueue.");
  this->blocked = false;
  assert(!this->drainEvent.scheduled() &&
         "Drain event should have not been scheduled.");
  this->drain();
}

void GemForgeDcachePortImpl::drain() {
  assert(!this->blocked && "drain while blocked.");
  if (this->blockedQueue.empty()) {
    return;
  }
  // Update the used port info.
  if (this->curCycle != cpu->curCycle()) {
    this->curCycle = cpu->curCycle();
    this->numUsedPorts = 0;
  }
  // Drain the blocked queue.
  while (!this->blockedQueue.empty() && !this->blocked &&
         this->numUsedPorts < this->numPorts) {
    auto pkt = this->blockedQueue.front().first;

    DPRINTF(GemForgeDcachePort, "Try to drain %s.\n", pkt->print());

    auto paddr = pkt->req->getPaddr();
    auto paddrLine = paddr & (~(cpu->system->cacheLineSize() - 1));

    /**
     * Block any packets to line that has been locked by RMW, except
     * the RMWWrite.
     */
    if (this->lockedRMWLinePAddr.count(paddrLine)) {
      if (!(pkt->req->isLockedRMW() && pkt->isWrite())) {
        // This is not the RMWWrite packet we are expecting, we have to
        // block the port, until RMWWrite comes. Notice that we do not
        // schedule drain event. When the RMWWrite comes,
        // sendTimingReqVirtual() will call me.
        DPRINTF(GemForgeDcachePort, "Skipped as not RMWWrite.\n");
        return;
      }
    }

    // Use the base class function.
    auto succeed = this->port->sendTimingReq(pkt);
    if (succeed) {
      this->numUsedPorts++;
      if (GemForgePacketHandler::isGemForgePacket(pkt)) {
        GemForgePacketHandler::issueToMemory(cpu->getCPUDelegator(), pkt);
      }

      /**
       * Maintain the lockedRMW state.
       */
      if (pkt->req->isLockedRMW()) {
        if (pkt->isRead()) {
          assert(this->lockedRMWLinePAddr.count(paddrLine) == 0 &&
                 "Already locked by RMWRead.");
          DPRINTF(GemForgeDcachePort, "Locked %#x.\n", paddrLine);
          this->lockedRMWLinePAddr.insert(paddrLine);
        } else {
          assert(pkt->isWrite() && "Invalid RMW req.");
          assert(this->lockedRMWLinePAddr.count(paddrLine) == 1 &&
                 "RMWWrite when unlocked.");
          DPRINTF(GemForgeDcachePort, "Unlocked %#x.\n", paddrLine);
          this->lockedRMWLinePAddr.erase(paddrLine);
        }
      }

      DPRINTF(GemForgeDcachePort, "Drained.\n");
      this->blockedQueue.pop_front();
    } else {
      // Blocked.
      DPRINTF(GemForgeDcachePort, "Blocked.\n");
      this->blocked = true;
    }
  }
  if (this->blocked) {
    // Blocked -- cancel future drainEvent.
    if (this->drainEvent.scheduled()) {
      cpu->deschedule(this->drainEvent);
    }
  } else if (!this->blockedQueue.empty()) {
    // We haven't finished.
    if (this->drainEvent.scheduled()) {
      cpu->deschedule(this->drainEvent);
    }
    cpu->schedule(this->drainEvent, cpu->clockEdge(Cycles(1)));
  }
}
} // namespace GemForge