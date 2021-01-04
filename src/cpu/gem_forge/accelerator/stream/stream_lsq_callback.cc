
#include "stream_lsq_callback.hh"

#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"

#include "debug/StreamBase.hh"
#define DEBUG_TYPE StreamBase
#include "stream_log.hh"

bool StreamLQCallback::getAddrSize(Addr &addr, uint32_t &size) const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  // Check if the address is ready.
  if (!this->element->isAddrReady) {
    return false;
  }
  addr = this->element->addr;
  size = this->element->size;
  return true;
}

bool StreamLQCallback::hasNonCoreDependent() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  return this->element->stream->hasNonCoreDependent();
}

bool StreamLQCallback::isIssued() const {
  /**
   * So far the element is considered issued when its address is ready.
   */
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  return this->element->isAddrReady;
}

bool StreamLQCallback::isValueReady() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");

  /**
   * We can directly check for element->isValueReady, but instead we
   * call areUsedStreamReady() so that StreamEngine can mark the
   * firstCheckCycle for the element, hence it can throttle the stream.
   */
  return this->element->se->areUsedStreamsReady(this->args);
}

void StreamLQCallback::RAWMisspeculate() {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  /**
   * Disable this for now.
   */
  // cpu->getIEWStage().misspeculateInst(userInst);
  this->element->se->RAWMisspeculate(this->element);
}

bool StreamLQCallback::bypassAliasCheck() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  // Only bypass alias check if the stream is marked FloatManual.
  return this->element->stream->getFloatManual();
}

StreamSQCallback::StreamSQCallback(StreamElement *_element,
                                   uint64_t _userSeqNum, Addr _userPC,
                                   const std::vector<uint64_t> &_usedStreamIds)
    : element(_element), FIFOIdx(_element->FIFOIdx),
      usedStreamIds(_usedStreamIds), args(_userSeqNum, _userPC, usedStreamIds) {
  /**
   * If the StoreStream is floated, it is possible that there are
   * still some SQCallbacks for the first few elements.
   */
  if (this->element->dynS->offloadedToCache) {
    S_ELEMENT_PANIC(this->element,
                    "StoreStream floated with outstanding SQCallback.");
  }
}

bool StreamSQCallback::getAddrSize(Addr &addr, uint32_t &size) const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  // Check if the address is ready.
  if (!this->element->isAddrReady) {
    return false;
  }
  addr = this->element->addr;
  size = this->element->size;
  return true;
}

bool StreamSQCallback::hasNonCoreDependent() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  return this->element->stream->hasNonCoreDependent();
}

bool StreamSQCallback::isIssued() const {
  /**
   * Store Request is issued by core, not stream engine.
   */
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  return false;
}

bool StreamSQCallback::isValueReady() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");

  /**
   * We can directly check for element->isValueReady, but instead we
   * call areUsedStreamReady() so that StreamEngine can mark the
   * firstCheckCycle for the element, hence it can throttle the stream.
   */
  return this->element->se->areUsedStreamsReady(this->args);
}

const uint8_t *StreamSQCallback::getValue() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  assert(this->isValueReady() && "GetValue before it's ready.");
  assert(this->usedStreamIds.size() == 1 && "GetValue for multiple streams.");
  return this->element->getValuePtrByStreamId(this->usedStreamIds.front());
}

void StreamSQCallback::RAWMisspeculate() {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  /**
   * SQCallback never triggers RAW misspeculation.
   */
  return;
}

bool StreamSQCallback::bypassAliasCheck() const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  // Only bypass alias check if the stream is marked FloatManual.
  return this->element->stream->getFloatManual();
}

bool StreamSQDeprecatedCallback::getAddrSize(Addr &addr, uint32_t &size) {
  // Check if the address is ready.
  if (!this->element->isAddrReady) {
    return false;
  }
  addr = this->element->addr;
  size = this->element->size;
  return true;
}

void StreamSQDeprecatedCallback::writeback() {
  // Start inform the stream engine to write back.
  this->element->se->writebackElement(this->element, this->storeInst);
}

bool StreamSQDeprecatedCallback::isWritebacked() {
  assert(this->element->inflyWritebackMemAccess.count(this->storeInst) != 0 &&
         "Missing writeback StreamMemAccess?");
  // Check if all the writeback accesses are done.
  return this->element->inflyWritebackMemAccess.at(this->storeInst).empty();
}

void StreamSQDeprecatedCallback::writebacked() {
  // Remember to clear the inflyWritebackStreamAccess.
  assert(this->element->inflyWritebackMemAccess.count(this->storeInst) != 0 &&
         "Missing writeback StreamMemAccess?");
  this->element->inflyWritebackMemAccess.erase(this->storeInst);
  // Remember to change the status of the stream store to committed.
  auto cpu = this->element->se->cpu;
  auto storeInstId = this->storeInst->getId();
  auto status = cpu->getInflyInstStatus(storeInstId);
  assert(status == LLVMTraceCPU::InstStatus::COMMITTING &&
         "Writebacked instructions should be committing.");
  cpu->updateInflyInstStatus(storeInstId, LLVMTraceCPU::InstStatus::COMMITTED);
}