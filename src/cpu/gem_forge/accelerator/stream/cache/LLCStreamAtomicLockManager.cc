#include "LLCStreamAtomicLockManager.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "debug/StreamRangeSync.hh"
#define DEBUG_TYPE StreamRangeSync
#include "../stream_log.hh"

#define ALM_ELEMENT_DPRINTF(element, format, args...)                          \
  LLC_SE_DPRINTF("%s%llu-: " format, (element)->dynStreamId, (element)->idx,   \
                 ##args)
#define ALM_ELEMENT_PANIC(element, format, args...)                            \
  LLC_SE_PANIC("%s%llu-: " format, (element)->dynStreamId, (element)->idx,     \
               ##args)

LLCStreamAtomicLockManager::LLCStreamAtomicLockManager(LLCStreamEngine *_se)
    : se(_se),
      commitPendingOpsEvent([this]() -> void { this->commitPendingOps(); },
                            _se->controller->name() +
                                ".stream_atomic_locker.commitPendingOps") {
  const auto &lockType = se->controller->getStreamAtomicLockType();
  if (lockType == "multi-reader") {
    this->lockType = LockType::MultpleReadersSingleWriterLock;
  } else if (lockType == "single") {
    this->lockType = LockType::SingleLock;
  }
}

void LLCStreamAtomicLockManager::enqueue(Addr paddr, int size,
                                         LLCStreamElementPtr element,
                                         bool memoryModified) {
  // Atomics is also considered as computation.
  this->se->controller->m_statLLCPerformedAtomics++;
  this->se->controller->m_statLLCScheduledComputation++;
  auto paddrQueue = this->getPAddrQueue(paddr);
  auto addrQueueIter =
      this->addrQueueMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(paddrQueue),
                   std::forward_as_tuple())
          .first;
  auto &lockQueue = addrQueueIter->second;
  if (!lockQueue.queue.empty()) {
    this->se->controller->m_statLLCLineConflictAtomics++;
  }
  bool foundRealConflict = false;
  bool foundRealXAWConflict = false;
  bool foundXAWConflict = false;
  for (const auto &op : lockQueue.queue) {
    if (!(op.paddr >= paddr + size || paddr >= op.paddr + op.size)) {
      // This is real conflict.
      foundRealConflict = true;
      if (op.memoryModified) {
        foundRealXAWConflict = true;
      }
    }
    if (op.memoryModified) {
      foundXAWConflict = true;
    }
  }
  if (foundRealConflict) {
    this->se->controller->m_statLLCRealConflictAtomics++;
  }
  if (foundRealXAWConflict) {
    this->se->controller->m_statLLCRealXAWConflictAtomics++;
  }
  if (foundXAWConflict) {
    this->se->controller->m_statLLCXAWConflictAtomics++;
  }
  // Simply push to the back.
  AtomicStreamOp newOp;
  newOp.paddr = paddr;
  newOp.size = size;
  newOp.element = element;
  newOp.memoryModified = memoryModified;
  newOp.enqueueCycle = this->se->controller->curCycle();
  ALM_ELEMENT_DPRINTF(
      element,
      "[AtomicLock] Queue %#x Enqueue %#x. Modified %d. Existing Ops %d.\n",
      paddrQueue, element.get(), memoryModified, lockQueue.queue.size());
  lockQueue.queue.push_back(newOp);
  this->tryToLockOps(addrQueueIter);
}

void LLCStreamAtomicLockManager::commit(
    Addr paddr, int size, LLCStreamElementPtr element,
    bool shouldAckAfterUnlock, const DynamicStreamSliceId &ackSliceId) {

  auto paddrQueue = this->getPAddrQueue(paddr);
  ALM_ELEMENT_DPRINTF(
      element, "[AtomicLock] Queue %#x RecvCommit. ShouldAckAfterUnlock %d.\n",
      paddrQueue, shouldAckAfterUnlock);

  auto addrQueueIter = this->addrQueueMap.find(paddrQueue);
  if (addrQueueIter == this->addrQueueMap.end()) {
    LLC_ELEMENT_PANIC(element, "[AtomicLock] No LockQueue found.");
  }
  auto &lockQueue = addrQueueIter->second;
  bool foundAtomicOp = false;
  for (auto &op : lockQueue.queue) {
    if (op.element->dynStreamId.isSameStaticStream(element->dynStreamId)) {
      /**
       * Sanity check that previous dynamic stream'e elements are committed.
       */
      if (op.element->dynStreamId.streamInstance <
          element->dynStreamId.streamInstance) {
        if (!op.committed) {
          ALM_ELEMENT_PANIC(op.element,
                            "[AtomicLock] Queue %#x Elements from previous "
                            "DynStream is not committed.",
                            paddrQueue);
        }
      }
    }
    if (op.paddr == paddr && op.size == size && op.element == element) {
      // Found it.
      if (op.committed) {
        LLC_ELEMENT_PANIC(element, "[AtomicLock] Already committed.");
      }
      foundAtomicOp = true;
      op.recvCommitCycle = this->se->controller->curCycle();
      op.shouldAckAfterUnlock = shouldAckAfterUnlock;
      op.ackSliceId = ackSliceId;
      this->se->controller->m_statLLCCommittedAtomics++;
      if (op.shouldAckAfterUnlock) {
        if (op.locked) {
          this->tryToCommitOp(op);
        }
      } else {
        /**
         * If it's not our responsibility to send back Ack, we should just
         * immediately commit.
         */
        this->commitOp(op);
      }
      break;
    }
  }
  if (!foundAtomicOp) {
    LLC_ELEMENT_PANIC(element, "[AtomicLock] Missing AtomicOp in LockQueue.");
  }
  this->unlockCommittedOps(addrQueueIter);
}

void LLCStreamAtomicLockManager::unlockCommittedOps(
    AddrQueueMapIter addrQueueIter) {

  auto paddrQueue = addrQueueIter->first;
  auto &queue = addrQueueIter->second.queue;
  while (!queue.empty()) {
    auto &op = queue.front();
    if (!op.committed) {
      // Cannot unlock.
      break;
    }
    // Unlock the line for this op.
    ALM_ELEMENT_DPRINTF(op.element,
                        "[AtomicLock] Queue %#x Unlock. Cycles since enqueue "
                        "%llu. Waiting Ops %llu.\n",
                        paddrQueue,
                        this->se->controller->curCycle() - op.enqueueCycle,
                        queue.size() - 1);
    if (!op.locked) {
      LLC_ELEMENT_PANIC(op.element, "[AtomicLock] Unlock before lock.\n");
    }
    this->se->controller->m_statLLCUnlockedAtomics++;
    auto &statistic = op.element->S->statistic;
    statistic.numFloatAtomic++;
    statistic.numFloatAtomicRecvCommitCycle +=
        op.recvCommitCycle - op.enqueueCycle;
    statistic.numFloatAtomicWaitForCommitCycle +=
        op.commitCycle - op.enqueueCycle;
    statistic.numFloatAtomicWaitForLockCycle += op.lockCycle - op.enqueueCycle;
    statistic.numFloatAtomicWaitForUnlockCycle +=
        this->se->controller->curCycle() - op.enqueueCycle;

    queue.pop_front();
    // Lock for the next op.
    if (!queue.empty()) {
      auto &nextOp = queue.front();
      if (!nextOp.locked) {
        this->lockForOp(nextOp);
      }
    }
  }

  if (queue.empty()) {
    LLC_SE_DPRINTF("[AtomicLock] Queue %#x Cleared.\n", paddrQueue);
    this->addrQueueMap.erase(addrQueueIter);
  } else {
    // We check if there are more ops to lock.
    this->tryToLockOps(addrQueueIter);
  }
}

void LLCStreamAtomicLockManager::lockForOp(AtomicStreamOp &op) {
  if (!op.element) {
    panic("[AtomicLock] Missing element.");
  }
  if (op.locked) {
    LLC_ELEMENT_PANIC(op.element, "[AtomicLock] Already locked.\n");
  }
  auto paddrQueue = this->getPAddrQueue(op.paddr);
  LLC_ELEMENT_DPRINTF(op.element, "[AtomicLock] Queue %#x Lock. Modified %d.\n",
                      paddrQueue, op.memoryModified);
  op.locked = true;
  op.lockCycle = this->se->controller->curCycle();
  this->se->controller->m_statLLCLockedAtomics++;
  if (op.recvCommitCycle != 0 && !op.committed) {
    this->tryToCommitOp(op);
  }
}

void LLCStreamAtomicLockManager::tryToLockOps(AddrQueueMapIter addrQueueIter) {
  auto &queue = addrQueueIter->second.queue;
  if (queue.empty()) {
    return;
  }
  auto &firstOp = queue.front();
  // We can always lock for the first op.
  if (!firstOp.locked) {
    this->lockForOp(firstOp);
  }
  /**
   * Depending on our lock type, we may be able to process more ops.
   */
  if (this->lockType == LockType::SingleLock) {
    return;
  } else if (this->lockType == LockType::MultpleReadersSingleWriterLock) {
    if (firstOp.memoryModified) {
      // This operation would change the memory.
      return;
    }
    // Lock other nop operations.
    auto iter = std::next(queue.begin());
    while (iter != queue.end() && !iter->memoryModified) {
      if (!iter->locked) {
        this->lockForOp(*iter);
      }
      ++iter;
    }
  }
}

void LLCStreamAtomicLockManager::tryToCommitOp(AtomicStreamOp &op) {
  /**
   * So far our hack implementation will assume lock when enqueuing.
   * Now the op should be locked, we charge both WaitForCommit and WaitForLock
   * cycles.
   */
  assert(op.locked && "Try to commit an op without lock.");
  assert(op.shouldAckAfterUnlock && "Should not model the queue.");
  auto curCycle = this->se->controller->curCycle();
  auto waitForLockCycle = op.lockCycle - op.enqueueCycle;
  auto waitForCommitCycle = op.recvCommitCycle - op.enqueueCycle;
  auto readyCycle = waitForLockCycle + waitForCommitCycle + op.enqueueCycle;
  if (readyCycle <= curCycle) {
    // We can immediately commit.
    this->commitOp(op);
  } else {
    this->pushPendingCommitOp(op, readyCycle);
  }
}

void LLCStreamAtomicLockManager::commitOp(AtomicStreamOp &op) {
  auto paddrQueue = this->getPAddrQueue(op.paddr);
  ALM_ELEMENT_DPRINTF(op.element, "[AtomicLock] Queue %#x Commit.\n",
                      paddrQueue);
  op.commitCycle = this->se->controller->curCycle();
  op.committed = true;
  /**
   * Send back the Ack when the StreamAtomicOp is unlocked.
   */
  if (op.shouldAckAfterUnlock) {
    ALM_ELEMENT_DPRINTF(op.element,
                        "[AtomicLock] Queue %#x Send back StreamAck.\n",
                        paddrQueue);
    this->se->issueStreamAckToMLC(op.ackSliceId, false /* forceIdea */);
  }
}

void LLCStreamAtomicLockManager::pushPendingCommitOp(AtomicStreamOp &op,
                                                     Cycles readyCycle) {
  assert(readyCycle != this->se->controller->curCycle() &&
         "This op should commit immediately.");
  auto iter = this->pendingCommitOps.begin();
  auto end = this->pendingCommitOps.end();
  while (iter != end) {
    if (iter->readyCycle > readyCycle) {
      break;
    }
    ++iter;
  }
  PendingCommitOp ackOp(op, readyCycle);

  Addr paddrQueue = this->getPAddrQueue(op.paddr);
  ALM_ELEMENT_DPRINTF(
      op.element, "[AtomicLock] Queue %#x Commit delayed by %llu cycles.\n",
      paddrQueue, readyCycle - this->se->controller->curCycle());

  bool insertAtFront = (iter == this->pendingCommitOps.begin());
  this->pendingCommitOps.insert(iter, ackOp);

  if (insertAtFront) {
    // We have to reschedule the event to readyCycle.
    if (this->commitPendingOpsEvent.scheduled()) {
      this->se->controller->deschedule(this->commitPendingOpsEvent);
    }
    this->se->controller->schedule(
        this->commitPendingOpsEvent,
        this->se->controller->cyclesToTicks(readyCycle));
  }
}

void LLCStreamAtomicLockManager::commitPendingOps() {
  if (this->pendingCommitOps.empty()) {
    panic("No pending atomics to commit.");
  }
  auto iter = this->pendingCommitOps.begin();
  auto end = this->pendingCommitOps.end();
  auto curCycle = this->se->controller->curCycle();
  std::set<Addr> changedQueue;
  while (iter != end && curCycle >= iter->readyCycle) {
    auto &op = iter->op;
    auto paddrQueue = this->getPAddrQueue(op.paddr);
    this->commitOp(op);
    changedQueue.insert(paddrQueue);
    iter = this->pendingCommitOps.erase(iter);
  }

  if (!this->pendingCommitOps.empty() &&
      !this->commitPendingOpsEvent.scheduled()) {
    auto firstReadyCycle = this->pendingCommitOps.front().readyCycle;
    this->se->controller->schedule(
        this->commitPendingOpsEvent,
        this->se->controller->cyclesToTicks(firstReadyCycle));
  }

  for (auto paddrQueue : changedQueue) {
    auto iter = this->addrQueueMap.find(paddrQueue);
    assert(iter != this->addrQueueMap.end());
    this->unlockCommittedOps(iter);
  }
}