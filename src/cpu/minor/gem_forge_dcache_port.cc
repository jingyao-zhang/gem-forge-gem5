#include "lsq.hh"

#include "cpu/gem_forge/gem_forge_packet_handler.hh"
#include "minor_cpu_delegator.hh"

#include "debug/MinorMem.hh"

/*********************************************************************
 * Implementation of the GemForgeDcachePort in the LSQ.
 ********************************************************************/

namespace Minor {

bool LSQ::GemForgeDcachePort::sendTimingReqVirtual(PacketPtr pkt, bool isCore) {
  // Update the used port info.
  if (this->curCycle != cpu.curCycle()) {
    this->curCycle = cpu.curCycle();
    this->numUsedPorts = 0;
  }

  DPRINTF(MinorMem, "Receive isCore %d, %s.\n", isCore, pkt->print());
  assert(this->blockedQueue.size() < 1024 && "Too many blocked packets.");

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
      DPRINTF(MinorMem, "Skip core req %s.\n", iter->first->print());
      ++iter;
    }
    // Insert.
    this->blockedQueue.emplace(iter, pkt, isCore);
  } else {
    // Simply insert at the end.
    DPRINTF(MinorMem, "Insert at the end.\n");
    this->blockedQueue.emplace_back(pkt, isCore);
  }

  /**
   * Special sanity check for RMWWrite.
   */
  if (pkt->req->isLockedRMW() && pkt->isWrite()) {
    assert(isCore && "RMWWrite should be from core.");
    auto paddr = pkt->req->getPaddr();
    auto paddrLine = paddr & (~(cpu.system->cacheLineSize() - 1));
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
    if (Debug::MinorMem) {
      if (this->blocked) {
        DPRINTF(MinorMem, "Blocked.\n");
      } else if (this->numUsedPorts == this->numPorts) {
        DPRINTF(MinorMem, "Used all %d ports.\n", this->numPorts);
      }
    }
    // If not the first reason, schedule the drain event.
    if (!this->blocked && !this->drainEvent.scheduled()) {
      cpu.schedule(this->drainEvent, cpu.clockEdge(Cycles(1)));
    }
  } else {
    // We can drain immediately.
    if (this->drainEvent.scheduled()) {
      cpu.deschedule(this->drainEvent);
    }
    this->drain();
  }
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
  if (this->blockedQueue.empty()) {
    return;
  }
  // Update the used port info.
  if (this->curCycle != cpu.curCycle()) {
    this->curCycle = cpu.curCycle();
    this->numUsedPorts = 0;
  }
  // Drain the blocked queue.
  while (!this->blockedQueue.empty() && !this->blocked &&
         this->numUsedPorts < this->numPorts) {
    auto pkt = this->blockedQueue.front().first;

    DPRINTF(MinorMem, "Try to drain %s.\n", pkt->print());

    auto paddr = pkt->req->getPaddr();
    auto paddrLine = paddr & (~(cpu.system->cacheLineSize() - 1));

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
        DPRINTF(MinorMem, "Skipped as not RMWWrite.\n");
        return;
      }
    }

    // Use the base class function.
    auto succeed = DcachePort::sendTimingReq(pkt);
    if (succeed) {
      this->numUsedPorts++;
      if (GemForgePacketHandler::isGemForgePacket(pkt)) {
        GemForgePacketHandler::issueToMemory(cpu.cpuDelegator.get(), pkt);
      }

      /**
       * Maintain the lockedRMW state.
       */
      if (pkt->req->isLockedRMW()) {
        if (pkt->isRead()) {
          assert(this->lockedRMWLinePAddr.count(paddrLine) == 0 &&
                 "Already locked by RMWRead.");
          DPRINTF(MinorMem, "Locked %#x.\n", paddrLine);
          this->lockedRMWLinePAddr.insert(paddrLine);
        } else {
          assert(pkt->isWrite() && "Invalid RMW req.");
          assert(this->lockedRMWLinePAddr.count(paddrLine) == 1 &&
                 "RMWWrite when unlocked.");
          DPRINTF(MinorMem, "Unlocked %#x.\n", paddrLine);
          this->lockedRMWLinePAddr.erase(paddrLine);
        }
      }

      DPRINTF(MinorMem, "Drained.\n");
      this->blockedQueue.pop_front();
    } else {
      // Blocked.
      DPRINTF(MinorMem, "Blocked.\n");
      this->blocked = true;
    }
  }
  if (this->blocked) {
    // Blocked -- cancel future drainEvent.
    if (this->drainEvent.scheduled()) {
      cpu.deschedule(this->drainEvent);
    }
  } else if (!this->blockedQueue.empty()) {
    // We haven't finished.
    if (this->drainEvent.scheduled()) {
      cpu.deschedule(this->drainEvent);
    }
    cpu.schedule(this->drainEvent, cpu.clockEdge(Cycles(1)));
  }
}
} // namespace Minor