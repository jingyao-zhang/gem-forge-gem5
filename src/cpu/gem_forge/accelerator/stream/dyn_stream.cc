#include "dyn_stream.hh"
#include "stream.hh"
#include "stream_element.hh"

#include "debug/StreamEngine.hh"
#define DEBUG_TYPE StreamEngine
#include "stream_log.hh"

DynamicStream::DynamicStream(const DynamicStreamId &_dynamicStreamId,
                             uint64_t _configSeqNum, Cycles _configCycle,
                             ThreadContext *_tc,
                             const FIFOEntryIdx &_prevFIFOIdx,
                             StreamEngine *_se)
    : dynamicStreamId(_dynamicStreamId), configSeqNum(_configSeqNum),
      configCycle(_configCycle), tc(_tc), prevFIFOIdx(_prevFIFOIdx),
      FIFOIdx(_dynamicStreamId, _configSeqNum) {
  this->tail = new StreamElement(_se);
  this->head = this->tail;
  this->stepped = this->tail;
}

DynamicStream::~DynamicStream() {
  delete this->tail;
  this->tail = nullptr;
  this->head = nullptr;
  this->stepped = nullptr;
}

StreamElement *DynamicStream::getElementByIdx(uint64_t elementIdx) const {
  for (auto element = this->tail->next; element != nullptr;
       element = element->next) {
    if (element->FIFOIdx.entryIdx == elementIdx) {
      return element;
    }
  }
  return nullptr;
}

StreamElement *DynamicStream::getPrevElement(StreamElement *element) {
  assert(element->FIFOIdx.streamId == this->dynamicStreamId &&
         "Element is not mine.");
  for (auto prevElement = this->tail; prevElement != nullptr;
       prevElement = prevElement->next) {
    if (prevElement->next == element) {
      return prevElement;
    }
  }
  assert(false && "Failed to find the previous element.");
}

StreamElement *DynamicStream::getFirstUnsteppedElement() {
  if (this->allocSize <= this->stepSize) {
    return nullptr;
  }
  auto element = this->stepped->next;
  // * Notice the element is guaranteed to be not stepped.
  assert(!element->isStepped && "Dispatch user to stepped stream element.");
  return element;
}

StreamElement *DynamicStream::releaseElementUnstepped() {
  /**
   * Make sure we release in reverse order.
   */
  auto prevElement = this->stepped;
  auto releaseElement = this->stepped->next;
  assert(releaseElement && "Missing unstepped element.");
  while (releaseElement->next) {
    prevElement = releaseElement;
    releaseElement = releaseElement->next;
  }
  assert(releaseElement == this->head &&
         "Head should point to the last element.");

  // This should be unused.
  assert(!releaseElement->isStepped && "Release stepped element.");
  assert(!releaseElement->isFirstUserDispatched() &&
         "Release unstepped but used element.");

  prevElement->next = releaseElement->next;
  this->allocSize--;
  this->head = prevElement;
  /**
   * Since this element is released as unstepped,
   * we need to reverse the FIFOIdx so that if we misspeculated,
   * new elements can be allocated with correct FIFOIdx.
   */
  this->FIFOIdx.prev();
  return releaseElement;
}

void DynamicStream::updateReleaseCycle(Cycles releaseCycle, bool late) {
  this->numReleaseElement++;
  if (late) {
    this->lateElementCount++;
  }
  if (this->numReleaseElement % DynamicStream::HistoryWindowSize == 0) {
    if (this->numReleaseElement >= 3 * DynamicStream::HistoryWindowSize) {
      // Time to update.
      this->avgTurnAroundCycle =
          Cycles((releaseCycle - this->lastReleaseCycle) /
                 DynamicStream::HistoryWindowSize);
      this->numLateElement = this->lateElementCount;
    }
    // Time to reset.
    this->lastReleaseCycle = releaseCycle;
    this->lateElementCount = 0;
  }
}

void DynamicStream::dump() const {
  inform("DynS %llu total %d step %3d allocated %3d. =======\n",
         this->dynamicStreamId.streamInstance, this->totalTripCount,
         this->stepSize, this->allocSize);
  std::stringstream ss;
  auto element = this->tail;
  while (element != this->head) {
    element = element->next;
    ss << element->FIFOIdx.entryIdx << '('
       << static_cast<int>(element->isAddrReady)
       << static_cast<int>(element->isValueReady) << ')';
    for (auto baseElement : element->baseElements) {
      ss << '.' << baseElement->FIFOIdx.entryIdx;
    }
    ss << ' ';
  }
  inform("%s\n", ss.str().c_str());
}