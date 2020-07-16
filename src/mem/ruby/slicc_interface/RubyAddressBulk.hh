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
   */
  bool empty() const { return this->addresses.empty(); }
  int size() const { return this->addresses.size(); }
  Addr getAt(int i) const { return this->addresses.at(i); }

  bool isInBulkTransition() const { return this->bulkTransitionStarted; }
  void startBulkTransition() {
    assert(!this->isInBulkTransition());
    assert(!this->empty());
    this->bulkTransitionStarted = true;
    this->currentIdxInBulk = 0;
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
    if (this->currentBulkMsg) {
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
};

#endif