#ifndef __MEM_RUBY_SLICC_INTERFACE_RUBY_ADDRESS_BULK_HH__
#define __MEM_RUBY_SLICC_INTERFACE_RUBY_ADDRESS_BULK_HH__

#include "Message.hh"

#include <vector>

/**
 * Represent an address bulk (multiple addresses), as well as
 * intermediate state for the cache controller.
 * This is used to implement bulk prefetch.
 */
struct RubyAddressBulk {
public:
  void push(Addr addr) {
    assert(!this->isInBulkTransition());
    this->addresses.push_back(addr);
  }

  /**
   * APIs for controller to implement bulk transistion.
   * It manages the state as:
   * 1. startBulkTransistion() will start bulk transition.
   * 2. The user iterates through bulk lines with getCurrentLineInBulk()
   *    and stepLineInBulk().
   * 3. The user will construct messages for the current line, and it
   *    uses mergeToBulkMessages() to merge to previous message. It is
   *    the user's responsibility to ensure that unmerged messages are
   *    enqueued.
   * 4. It is possible to exitBulkTransition() even when there are
   *    unprocessed lines in the bulk. In such case, the user can just
   *    start later.
   */
  bool empty() const { return this->addresses.empty(); }
  int size() const { return this->addresses.size(); }
  Addr getAt(int i) const {
    assert(i >= this->currentIdxInBulk);
    assert(i < this->size());
    return this->addresses.at(i);
  }

  bool isInBulkTransition() const { return this->bulkTransitionStarted; }
  void startBulkTransition() {
    assert(!this->isInBulkTransition());
    assert(!this->empty());
    assert(!this->currentBulkMsg);
    assert(this->currentIdxInBulk < this->size());
    this->bulkTransitionStarted = true;
  }
  void exitBulkTransition() {
    assert(this->isInBulkTransition());
    this->bulkTransitionStarted = false;
    this->currentBulkMsg = nullptr;
  }
  int getCurrentIdxInBulk() const {
    assert(this->currentIdxInBulk < this->size());
    return this->currentIdxInBulk;
  }
  Addr getCurrentLineInBulk() const {
    assert(this->currentIdxInBulk < this->size());
    return this->getAt(this->currentIdxInBulk);
  }
  bool isLastLineInBulk() const {
    assert(this->currentIdxInBulk < this->size());
    return this->currentIdxInBulk + 1 == this->size();
  }
  void stepLineInBulk() {
    assert(this->currentIdxInBulk < this->size());
    this->currentIdxInBulk++;
  }
  bool mergeToBulkMessage(MsgPtr msg) {
    if (this->currentBulkMsg && this->currentBulkMsgTick == curTick()) {
      const auto &dest1 = this->currentBulkMsg->getDestination();
      const auto &dest2 = msg->getDestination();
      if (dest1.isEqual(dest2) && dest1.count() == 1) {
        // We can merge these two.
        this->currentBulkMsg->chainMsg(msg);
        return true;
      }
    }
    // Otherwise, we take this as our new bulk message.
    this->currentBulkMsg = msg;
    this->currentBulkMsgTick = curTick();
    return false;
  }

private:
  std::vector<Addr> addresses;

  /**
   * State for bulk transistion in the controller.
   */
  bool bulkTransitionStarted = false;
  int currentIdxInBulk = 0;
  // We do not hold the ownership.
  MsgPtr currentBulkMsg = nullptr;
  Tick currentBulkMsgTick = 0;
};

#endif