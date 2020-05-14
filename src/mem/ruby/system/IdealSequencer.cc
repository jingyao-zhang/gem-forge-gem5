#include "IdealSequencer.hh"

#include "Sequencer.hh"

std::queue<IdealSequencer::IdealRubyRequest> IdealSequencer::globalQueue;
std::unordered_map<Addr, IdealSequencer *> IdealSequencer::globalLockedMap;
std::unordered_map<Addr, std::queue<IdealSequencer::IdealRubyRequest>>
    IdealSequencer::globalLockedQueue;
IdealSequencer::IdealDrainEvent *IdealSequencer::drainEvent = nullptr;

void IdealSequencer::pushRequest(RubyReqPtr req) {
  /**
   * All request has 1 cycle latency, except for access to a LockedRMW line,
   * which is pushed into the glboalLockedMap and drained later.
   * The line is locked after the request is pushed into the queue.
   */
  auto paddrLine = req->m_LineAddress;
  if (globalLockedMap.count(paddrLine)) {
    // This line is locked, check if this req will to unlock it.
    auto lockSeq = globalLockedMap.at(paddrLine);
    if (lockSeq && req->m_pkt->req->isLockedRMW() && req->m_pkt->isWrite()) {
      // This will unlock the line, continue to push to the queue.
    } else {
      // This should be blocked.
      globalLockedQueue.at(paddrLine).emplace(this, req);
      return;
    }
  }

  /**
   * The request should be pushed into the queue for draining.
   * So far we used fixed 1 cycle latency.
   * Also, if this is a LockedRMWRead, we lock the line.
   */
  this->globalQueue.emplace(this, req, this->seq->clockEdge(Cycles(1)));
  if (req->m_pkt->req->isLockedRMW() && req->m_pkt->isRead()) {
    auto locked = globalLockedMap.emplace(paddrLine, this).second;
    assert(locked && "This line is already locked.");
    // Try to initialize the blocked queue (there may already be some blocked
    // requests).
    globalLockedQueue.emplace(std::piecewise_construct,
                              std::forward_as_tuple(paddrLine),
                              std::forward_as_tuple());
  }

  // Schedule the drain event, since we are in the queue.
  if (!drainEvent->scheduled()) {
    this->seq->schedule(drainEvent, this->seq->clockEdge(Cycles(1)));
  }
}

const char *IdealSequencer::IdealDrainEvent::description() const {
  return "IdealSequencer.drain";
}

void IdealSequencer::IdealDrainEvent::process() {
  std::vector<Addr> unlockedAddrs;
  auto currentTick = curTick();
  while (!globalQueue.empty()) {
    const auto &req = globalQueue.front();
    auto paddrLine = req.req->m_LineAddress;
    if (req.readyTick > currentTick) {
      break;
    }
    /**
     * If this is a LockedRMWWrite, remember the unlocked address.
     */
    if (req.req->m_pkt->req->isLockedRMW() && req.req->m_pkt->isWrite()) {
      if (globalLockedMap.count(paddrLine)) {
        unlockedAddrs.push_back(paddrLine);
      }
    }
    // Call the sequencer's callback. Just use the WTData as the dummy block,
    // as the sequencer will access the backing storage for actual data.
    auto seq = req.seq->seq;
    auto reqType = req.req->m_Type;
    if (reqType == RubyRequestType_IFETCH || reqType == RubyRequestType_LD) {
      seq->readCallback(paddrLine, req.req->m_WTData);
    } else if (reqType == RubyRequestType_ST ||
               reqType == RubyRequestType_ATOMIC) {
      seq->writeCallback(paddrLine, req.req->m_WTData);
    } else {
      panic("Invalid RubyRequestType %s in IdealMode.\n", reqType);
    }

    globalQueue.pop();
  }

  // Process unlocked address.
  for (auto paddrLine : unlockedAddrs) {
    assert(globalLockedMap.count(paddrLine));
    globalLockedMap.erase(paddrLine);
    // Check if we have blocked other requests.
    auto &blockedQueue = globalLockedQueue.at(paddrLine);
    // We keep issuing until no more blocked requests, or locked again.
    while (!blockedQueue.empty() && globalLockedMap.count(paddrLine) == 0) {
      auto &req = blockedQueue.front();
      req.seq->pushRequest(req.req);
      blockedQueue.pop();
    }

    if (blockedQueue.empty() && globalLockedMap.count(paddrLine) == 0) {
      // No more blocked requests, release the blocked queue.
      globalLockedQueue.erase(paddrLine);
    }
  }

  // Finally, if we have more requests, schedule next drain event.
  if (!globalQueue.empty() && !drainEvent->scheduled()) {
    auto seq = globalQueue.front().seq->seq;
    seq->schedule(drainEvent, seq->clockEdge(Cycles(1)));
  }
}
