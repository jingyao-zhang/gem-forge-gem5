
#ifndef __CPU_GEM_FORGE_ACCELERATOR_LLC_STREAM_ATOMIC_LOCK_MANAGER_H__
#define __CPU_GEM_FORGE_ACCELERATOR_LLC_STREAM_ATOMIC_LOCK_MANAGER_H__

#include "LLCStreamEngine.hh"

#include <list>

class LLCStreamAtomicLockManager {
public:
  LLCStreamAtomicLockManager(LLCStreamEngine *_se);

  void enqueue(Addr paddr, int size, LLCStreamElementPtr element,
               bool memoryModified);
  void commit(Addr paddr, int size, LLCStreamElementPtr element,
              bool shouldAckAfterUnlock = false,
              const DynStreamSliceId &ackSliceId = DynStreamSliceId());

private:
  LLCStreamEngine *se;

  enum LockType {
    SingleLock,
    MultpleReadersSingleWriterLock,
  };
  LockType lockType = LockType::SingleLock;

  struct AtomicStreamOp {
    Addr paddr = 0;
    int size = 0;
    LLCStreamElementPtr element = nullptr;
    bool memoryModified = false;
    bool locked = false;
    // If committed, we can unlock the line.
    bool committed = false;
    // Stats for latency.
    Cycles enqueueCycle = Cycles(0);
    Cycles recvCommitCycle = Cycles(0);
    Cycles commitCycle = Cycles(0);
    Cycles lockCycle = Cycles(0);
    // Ack SliceId.
    bool shouldAckAfterUnlock = false;
    DynStreamSliceId ackSliceId;
  };

  using LockQueue = std::list<AtomicStreamOp>;
  using LockQueueIter = LockQueue::iterator;

  using AddrQueueMapT = std::unordered_map<Addr, LockQueue>;
  using AddrQueueMapIter = AddrQueueMapT::iterator;
  AddrQueueMapT addrQueueMap;

  /**
   * Global reverse map from element to the position in the LockQueue.
   * Used for deadlock detection oracle.
   * NOTE: References to element of std::unordered_map is not invalidated after
   * rehashing.
   * NOTE: Iterators to element of std::list is not invalidated after insertion.
   */
  struct GlobalLockQueuePosition {
    LLCStreamAtomicLockManager *manager;
    LockQueue &queue;
    LockQueueIter queueIter;
    GlobalLockQueuePosition(LLCStreamAtomicLockManager *_manager,
                            LockQueue &_queue, LockQueueIter _queueIter)
        : manager(_manager), queue(_queue), queueIter(_queueIter) {}
  };
  using ElementLockPositionMap =
      std::unordered_map<LLCStreamElementPtr, GlobalLockQueuePosition>;

  static ElementLockPositionMap elementQueuePositionMap;

  /**
   * There life cycle of an atomic:
   * 1. Enqueued into the lock queue.
   * 2. When it acquires the lock, switch to locked state and:
   *  - If it has already received the commit message, delay the commit by
   *    WaitForLock cycles.
   *  - Otherwise, keep wait for the commit message.
   * 3. When recv the commit message:
   *  - If locked, delay the commit by WaitForLock cycles.
   *  - Otherwise, wait for the lock.
   * 4. Release the lock when the delayed commit message is handled.
   */

  struct PendingCommitOp {
    /**
     * ! Be carefule with reference.
     */
    AtomicStreamOp &op;
    Cycles readyCycle;
    PendingCommitOp(AtomicStreamOp &_op, Cycles _readyCycle)
        : op(_op), readyCycle(_readyCycle) {}
  };

  std::list<PendingCommitOp> pendingCommitOps;
  void tryToCommitOp(AtomicStreamOp &op);
  void commitOp(AtomicStreamOp &op);
  void pushPendingCommitOp(AtomicStreamOp &op, Cycles readyCycle);
  void commitPendingOps();
  EventFunctionWrapper commitPendingOpsEvent;

  int curRemoteBank() const { return this->se->curRemoteBank(); }
  const char *curRemoteMachineType() const {
    return this->se->curRemoteMachineType();
  }

  Addr getPAddrQueue(Addr paddr) const {
    /**
     * So far we build the lock queue at line granularity.
     */
    return makeLineAddress(paddr);
  }

  /**
   * Advance the queue to unlock committed op.
   * Please be careful that this will modify the queue.
   */
  void unlockCommittedOps(AddrQueueMapIter addrQueueIter);

  /**
   * Lock the operation.
   */
  void tryToLockOps(AddrQueueMapIter addrQueueIter);
  void lockForOp(AtomicStreamOp &op);

  /**
   * Detect deadlock.
   * When pushing a new element, we perform DFS on all dependent elements, and
   * report a deadlock if we found a cycle.
   */
  void checkDeadlock(LLCStreamElementPtr newElement) const;
};

#endif