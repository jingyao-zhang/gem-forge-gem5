#include "dyn_stream.hh"
#include "stream.hh"
#include "stream_element.hh"
#include "stream_engine.hh"

#include "debug/StreamBase.hh"
#define DEBUG_TYPE StreamBase
#include "stream_log.hh"

DynamicStream::DynamicStream(Stream *_stream,
                             const DynamicStreamId &_dynamicStreamId,
                             uint64_t _configSeqNum, Cycles _configCycle,
                             ThreadContext *_tc, StreamEngine *_se)
    : stream(_stream), dynamicStreamId(_dynamicStreamId),
      configSeqNum(_configSeqNum), configCycle(_configCycle), tc(_tc),
      FIFOIdx(_dynamicStreamId, _configSeqNum) {
  this->tail = new StreamElement(_se);
  this->head = this->tail;
  this->stepped = this->tail;

  for (auto &hitPrivate : this->hitPrivateCacheHistory) {
    hitPrivate = false;
  }
  this->totalHitPrivateCache = 0;
}

DynamicStream::~DynamicStream() {
  delete this->tail;
  this->tail = nullptr;
  this->head = nullptr;
  this->stepped = nullptr;
}

bool DynamicStream::shouldCoreSEIssue() const {
  /**
   * If the stream has floated, and no core user/dependent streams here,
   * then we don't have to issue for the data.
   */
  if (!this->stream->hasCoreUser() && this->offloadedToCache) {
    // Check that dependent dynS all offloaded to cache.
    bool allDepSFloated = true;
    for (auto depS : this->stream->addrDepStreams) {
      const auto &depDynS = depS->getDynamicStream(this->configSeqNum);
      if (!depDynS.offloadedToCache) {
        allDepSFloated = false;
        break;
      }
    }
    if (allDepSFloated) {
      return false;
    }
  }
  return true;
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
  if (this->allocSize == this->stepSize) {
    return nullptr;
  }
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

  S_ELEMENT_DPRINTF(releaseElement,
                    "ReleaseElementUnstepped, isAddrReady %d.\n",
                    releaseElement->isAddrReady);
  // Check if the element is faulted.
  if (this->stream->isMemStream() && releaseElement->isAddrReady) {
    if (releaseElement->isValueFaulted(releaseElement->addr,
                                       releaseElement->size)) {
      this->stream->statistic.numFaulted++;
    }
  }
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

void DynamicStream::recordHitHistory(bool hitPrivateCache) {
  auto &record =
      this->hitPrivateCacheHistory.at(this->currentHitPrivateCacheHistoryIdx);
  if (record) {
    this->totalHitPrivateCache--;
    assert(this->totalHitPrivateCache >= 0);
  }
  record = hitPrivateCache;
  if (record) {
    this->totalHitPrivateCache++;
  }
  this->currentHitPrivateCacheHistoryIdx =
      (this->currentHitPrivateCacheHistoryIdx + 1) %
      this->hitPrivateCacheHistory.size();
  // Let's check if we have hitInPrivateCache a lot.
  if (this->offloadedToCacheAsRoot) {
    auto totalHitPrivateCache = this->getTotalHitPrivateCache();
    auto hitPrivateCacheHistoryWindowSize =
        this->getHitPrivateCacheHistoryWindowSize();
    if (totalHitPrivateCache == hitPrivateCacheHistoryWindowSize) {
      // Try to cancel this offloaded.
      S_DPRINTF(this->stream, "All Last %d Requests Hit in Private Cache.\n",
                totalHitPrivateCache);
      this->tryCancelFloat();
    }
  }
}

void DynamicStream::tryCancelFloat() {
  // So far we only have limited support cacelling stream floating.
  auto S = this->stream;
  if (!S->se->isStreamFloatCancelEnabled()) {
    // This feature is not enabled.
    return;
  }
  if (!S->addrDepStreams.empty()) {
    return;
  }
  if (S->enabledStoreFunc()) {
    return;
  }
  if (this->offloadedWithDependent) {
    return;
  }
  // Try cancel the whole coalesce group.
  auto sid = S->getCoalesceBaseStreamId();
  std::vector<DynamicStream *> endStreams;
  if (sid == 0) {
    // No coalesce group, just cancel myself.
    endStreams.push_back(this);
  } else {
    auto coalesceBaseS = S->se->getStream(sid);
    const auto &coalesceGroupS = coalesceBaseS->coalesceGroupStreams;
    for (auto &coalescedS : coalesceGroupS) {
      auto &dynS = coalescedS->getDynamicStream(this->configSeqNum);
      // Simply check that it has been floated.
      if (dynS.offloadedToCacheAsRoot && !dynS.offloadedWithDependent) {
        endStreams.push_back(&dynS);
      }
    }
  }
  // Construct the end ids.
  if (endStreams.empty()) {
    return;
  }
  std::vector<DynamicStreamId> endIds;
  for (auto endDynS : endStreams) {
    endIds.push_back(endDynS->dynamicStreamId);
    endDynS->cancelFloat();
  }
  S->se->sendStreamFloatEndPacket(endIds);
}

void DynamicStream::cancelFloat() {
  auto S = this->stream;
  S_DPRINTF(S, "Cancel FloatStream.\n");
  S_HACK(S, "Cancel FloatStream.\n");
  // We are no longer considered offloaded.
  this->offloadedToCache = false;
  this->offloadedToCacheAsRoot = false;
  S->statistic.numFloatCancelled++;
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
    for (auto baseElement : element->addrBaseElements) {
      ss << '.' << baseElement->FIFOIdx.entryIdx;
    }
    ss << ' ';
  }
  inform("%s\n", ss.str().c_str());
}