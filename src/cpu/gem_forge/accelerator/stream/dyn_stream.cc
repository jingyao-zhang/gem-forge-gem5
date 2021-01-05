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

void DynamicStream::addBaseDynStreams() {
  this->addAddrBaseDynStreams();
  this->addBackBaseDynStreams();
}

void DynamicStream::addAddrBaseDynStreams() {
  for (const auto &edge : this->stream->addrBaseEdges) {
    auto baseS = this->stream->se->getStream(edge.toStaticId);
    auto &baseDynS = baseS->getLastDynamicStream();
    DYN_S_DPRINTF(this->dynamicStreamId, "AddrBaseDynS %s Me %s.\n",
                  baseS->getStreamName(), this->dynamicStreamId.streamName);
    /**
     * If there are unstepped elements, we align to the first one.
     * If not (i.e. just created), we align to the next one.
     */
    auto alignBaseElementIdx = baseDynS.FIFOIdx.entryIdx;
    if (auto baseFirstUnsteppedElement = baseDynS.getFirstUnsteppedElement()) {
      alignBaseElementIdx = baseFirstUnsteppedElement->FIFOIdx.entryIdx;
    }
    /**
     * The reuse count is delayed as StreamConfig has not executed,
     * and we don't know the trip count yet.
     * Simply initialize it to 1, as the most general case.
     */
    auto reuseBaseElement = 1;
    this->addrBaseEdges.emplace_back(
        edge.toStaticId, baseDynS.dynamicStreamId.streamInstance,
        edge.fromStaticId, alignBaseElementIdx, reuseBaseElement);
  }
}

void DynamicStream::addBackBaseDynStreams() {
  for (const auto &edge : this->stream->backBaseEdges) {
    auto baseS = this->stream->se->getStream(edge.toStaticId);
    auto &baseDynS = baseS->getLastDynamicStream();
    DYN_S_DPRINTF(this->dynamicStreamId, "BackBaseDynS %s Me %s.\n",
                  baseS->getStreamName(), this->dynamicStreamId.streamName);
    /**
     * If there are unstepped elements, we align to the first one.
     * If not (i.e. just created), we align to the next one.
     */
    auto alignBaseElementIdx = baseDynS.FIFOIdx.entryIdx;
    if (auto baseFirstUnsteppedElement = baseDynS.getFirstUnsteppedElement()) {
      alignBaseElementIdx = baseFirstUnsteppedElement->FIFOIdx.entryIdx;
    }
    /**
     * For back dependence, the reuse count should always be 1.
     */
    auto reuseBaseElement = 1;
    this->backBaseEdges.emplace_back(
        edge.toStaticId, baseDynS.dynamicStreamId.streamInstance,
        edge.fromStaticId, alignBaseElementIdx, reuseBaseElement);
  }
}

void DynamicStream::configureAddrBaseDynStreamReuse() {
  for (auto &edge : this->addrBaseEdges) {
    auto baseS = this->stream->se->getStream(edge.baseStaticId);
    auto &baseDynS = baseS->getDynamicStreamByInstance(edge.baseInstanceId);
    if (baseS->getLoopLevel() == this->stream->getLoopLevel()) {
      this->configureAddrBaseDynStreamReuseSameLoop(edge, baseDynS);
    } else {
      this->configureAddrBaseDynStreamReuseOuterLoop(edge, baseDynS);
    }
    DYN_S_DPRINTF(this->dynamicStreamId, "Configure Reuse %llu to Base %s.\n",
                  edge.reuseBaseElement, baseS->getStreamName());
  }
}

void DynamicStream::configureAddrBaseDynStreamReuseSameLoop(
    StreamDepEdge &edge, DynamicStream &baseDynS) {
  auto baseS = baseDynS.stream;
  auto S = this->stream;
  if (baseS->stepRootStream == S->stepRootStream) {
    // We are synchronized by the same step root.
    edge.reuseBaseElement = 1;
  } else {
    // The other one must be a constant stream.
    // Reuse 0 means always reuse.
    assert(baseS->stepRootStream == nullptr && "Should be a constant stream.");
    edge.reuseBaseElement = 0;
  }
}

void DynamicStream::configureAddrBaseDynStreamReuseOuterLoop(
    StreamDepEdge &edge, DynamicStream &baseDynS) {
  auto baseS = baseDynS.stream;
  auto S = this->stream;
  auto baseLoopLevel = baseS->getLoopLevel();
  auto loopLevel = S->getLoopLevel();
  assert(baseLoopLevel < loopLevel && "Base should be outer.");
  assert(S->stepRootStream && "Miss StepRoot for the DepS.");
  auto &stepRootDynS = S->stepRootStream->getDynamicStream(this->configSeqNum);

  auto stepRootAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
      stepRootDynS.addrGenCallback);
  assert(stepRootAddrGen && "Should be linear addr gen.");
  DYN_S_DPRINTF(this->dynamicStreamId, "loopLevel %d.\n", loopLevel);
  auto reuse = stepRootAddrGen->getNestTripCount(
      stepRootDynS.addrGenFormalParams, loopLevel - baseLoopLevel);
  edge.reuseBaseElement = reuse;
}

bool DynamicStream::areNextBaseElementsAllocated() const {
  return this->areNextAddrBaseElementsAllocated() &&
         this->areNextBackBaseElementsAllocated();
}

bool DynamicStream::areNextAddrBaseElementsAllocated() const {
  auto S = this->stream;
  for (const auto &edge : this->addrBaseEdges) {
    auto baseS = S->se->getStream(edge.baseStaticId);
    const auto &baseDynS =
        baseS->getDynamicStreamByInstance(edge.baseInstanceId);
    // Let's compute the base element entryIdx.
    uint64_t baseElementIdx = edge.alignBaseElement;
    if (edge.reuseBaseElement != 0) {
      baseElementIdx += this->FIFOIdx.entryIdx / edge.reuseBaseElement;
    }
    // Try to find this element.
    if (baseDynS.FIFOIdx.entryIdx <= baseElementIdx) {
      DYN_S_DPRINTF(this->dynamicStreamId,
                    "NextElementIdx(%llu) BaseElementIdx(%llu) Not Ready, "
                    "Align(%llu), Reuse(%llu), BaseStream %s.\n",
                    this->FIFOIdx.entryIdx, baseElementIdx,
                    edge.alignBaseElement, edge.reuseBaseElement,
                    baseS->getStreamName());
      return false;
    }
    auto baseElement = baseDynS.getElementByIdx(baseElementIdx);
    if (!baseElement) {
      DYN_S_DPRINTF(this->dynamicStreamId,
                    "NextElementIdx(%llu) BaseElementIdx(%llu) Already "
                    "Released? Align(%llu), Reuse(%llu), BaseStream %s\n",
                    this->FIFOIdx.entryIdx, baseElementIdx,
                    edge.alignBaseElement, edge.reuseBaseElement,
                    baseS->getStreamName());
      DYN_S_DPRINTF(this->dynamicStreamId, "BaseDynS %s.\n",
                    baseDynS.dumpString());
    }
    assert(baseElement && "Base Element Already Released?");
    if (baseElement->isStepped) {
      DYN_S_DPRINTF(this->dynamicStreamId,
                    "NextElementIdx(%llu) BaseElementIdx(%llu) Already "
                    "Stepped? Align(%llu), Reuse(%llu), BaseStream %s\n",
                    this->FIFOIdx.entryIdx, baseElementIdx,
                    edge.alignBaseElement, edge.reuseBaseElement,
                    baseS->getStreamName());
    }
    assert(!baseElement->isStepped && "Base Element Already Stepped?");
  }
  return true;
}

bool DynamicStream::areNextBackBaseElementsAllocated() const {
  auto S = this->stream;
  // No back dependence for the first element.
  if (this->FIFOIdx.entryIdx == 0) {
    return true;
  }
  for (const auto &edge : this->backBaseEdges) {
    auto baseS = S->se->getStream(edge.baseStaticId);
    const auto &baseDynS =
        baseS->getDynamicStreamByInstance(edge.baseInstanceId);
    // Let's compute the base element entryIdx.
    uint64_t baseElementIdx = edge.alignBaseElement;
    assert(edge.reuseBaseElement == 1 && "BackEdge should have reuse 1.");
    baseElementIdx += this->FIFOIdx.entryIdx - 1;
    // Try to find this element.
    if (baseDynS.FIFOIdx.entryIdx <= baseElementIdx) {
      DYN_S_DPRINTF(this->dynamicStreamId,
                    "NextElementIdx(%llu) BackBaseElementIdx(%llu) Not Ready, "
                    "Align(%llu), Reuse(%llu), BaseStream %s.\n",
                    this->FIFOIdx.entryIdx, baseElementIdx,
                    edge.alignBaseElement, edge.reuseBaseElement,
                    baseS->getStreamName());
      return false;
    }
    auto baseElement = baseDynS.getElementByIdx(baseElementIdx);
    if (!baseElement) {
      DYN_S_DPRINTF(this->dynamicStreamId,
                    "NextElementIdx(%llu) BaseElementIdx(%llu) Already "
                    "Released? Align(%llu), Reuse(%llu), BaseStream %s\n",
                    this->FIFOIdx.entryIdx, baseElementIdx,
                    edge.alignBaseElement, edge.reuseBaseElement,
                    baseS->getStreamName());
      DYN_S_DPRINTF(this->dynamicStreamId, "BaseDynS %s.\n",
                    baseDynS.dumpString());
    }
    assert(baseElement && "Base Element Already Released?");
  }
  return true;
}

void DynamicStream::addBaseElements(StreamElement *newElement) {
  this->addAddrBaseElements(newElement);
  this->addBackBaseElements(newElement);
}

void DynamicStream::addAddrBaseElements(StreamElement *newElement) {
  for (const auto &edge : this->addrBaseEdges) {
    this->addAddrOrBackBaseElementEdge(newElement, edge);
  }
}

void DynamicStream::addBackBaseElements(StreamElement *newElement) {
  if (newElement->FIFOIdx.entryIdx == 0) {
    // No back dependence for the first element.
    return;
  }
  for (const auto &edge : this->backBaseEdges) {
    this->addAddrOrBackBaseElementEdge(newElement, edge, true /* isBack */);
  }
}

void DynamicStream::addAddrOrBackBaseElementEdge(StreamElement *newElement,
                                                 const StreamDepEdge &edge,
                                                 bool isBack) {
  auto S = this->stream;
  auto baseS = S->se->getStream(edge.baseStaticId);
  auto &baseDynS = baseS->getDynamicStreamByInstance(edge.baseInstanceId);
  // Let's compute the base element entryIdx.
  uint64_t baseElementIdx = edge.alignBaseElement;
  if (edge.reuseBaseElement != 0) {
    baseElementIdx += newElement->FIFOIdx.entryIdx / edge.reuseBaseElement;
  }
  if (isBack) {
    assert(baseElementIdx > 0 && "First element has no back dep.");
    baseElementIdx--;
  }
  S_ELEMENT_DPRINTF(
      newElement,
      "BaseElementIdx(%llu), Align(%llu), Reuse (%llu), BaseStream %s.\n",
      baseElementIdx, edge.alignBaseElement, edge.reuseBaseElement,
      baseS->getStreamName());
  // Try to find this element.
  auto baseElement = baseDynS.getElementByIdx(baseElementIdx);
  assert(baseElement && "Failed to find base element.");
  newElement->addrBaseElements.insert(baseElement);
}

bool DynamicStream::shouldCoreSEIssue() const {
  /**
   * If the stream has floated, and no core user/dependent streams here,
   * then we don't have to issue for the data.
   */
  if (!this->stream->hasCoreUser() && this->offloadedToCache) {
    // Check that dependent dynS all offloaded to cache.
    for (auto depS : this->stream->addrDepStreams) {
      const auto &depDynS = depS->getDynamicStream(this->configSeqNum);
      if (!depDynS.offloadedToCache) {
        return true;
      }
    }
    for (auto backDepS : this->stream->backDepStreams) {
      const auto &backDepDynS = backDepS->getDynamicStream(this->configSeqNum);
      if (!backDepDynS.offloadedToCache) {
        return true;
      }
    }
    return false;
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

void DynamicStream::computeElementValue(StreamElement *element) {

  assert(this->stream->isStoreStream() && this->stream->getEnabledStoreFunc());
  if (this->offloadedToCache) {
    // This stream is offloaded to cache.
    assert("This not handled yet.");
    return;
  }
  if (!element->isAddrReady) {
    S_ELEMENT_PANIC(element, "StoreFunc should have addr ready.");
  }
  // Check for value base element.
  if (!element->checkValueBaseElementsValueReady()) {
    S_ELEMENT_PANIC(element,
                    "StoreFunc with ValueBaseElement not value ready.");
  }
  // Get value for store func.
  auto getStoreFuncInput = [this, element](StaticId id) -> StreamValue {
    // Search the ValueBaseElements.
    auto baseS = element->se->getStream(id);
    for (const auto &baseE : element->valueBaseElements) {
      if (baseE.element->stream == baseS) {
        // Found it.
        StreamValue elementValue;
        baseE.element->getValueByStreamId(id, elementValue.uint8Ptr(),
                                          sizeof(elementValue));
        return elementValue;
      }
    }
    assert(false && "Failed to find value base element.");
  };
  auto params =
      convertFormalParamToParam(this->storeFormalParams, getStoreFuncInput);
  auto storeValue = this->storeCallback->invoke(params);

  S_ELEMENT_DPRINTF(element, "StoreValue %s.\n", storeValue);
  // Set the element with the value.
  element->setValue(element->addr, element->size, storeValue.uint8Ptr());
}

void DynamicStream::updateStatsOnReleaseStepElement(Cycles releaseCycle,
                                                    uint64_t vaddr, bool late) {
  this->numReleaseElement++;
  if (this->numReleaseElement == 1) {
    this->startVAddr = vaddr;
  }
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
  if (S->getEnabledStoreFunc()) {
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
  // We are no longer considered offloaded.
  this->offloadedToCache = false;
  this->offloadedToCacheAsRoot = false;
  S->statistic.numFloatCancelled++;
}

int32_t DynamicStream::getBytesPerMemElement() const {
  auto memElementSize = this->stream->getMemElementSize();
  if (auto linearAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
          this->addrGenCallback)) {
    auto absInnerStride =
        std::abs(linearAddrGen->getInnerStride(this->addrGenFormalParams));
    return absInnerStride > memElementSize ? memElementSize : absInnerStride;
  } else {
    return memElementSize;
  }
}

std::string DynamicStream::dumpString() const {
  std::stringstream ss;
  ss << "===== " << this->dynamicStreamId << " total " << this->totalTripCount
     << " step " << this->stepSize << " alloc " << this->allocSize << " max "
     << this->stream->maxSize << " ========\n";
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
  return ss.str();
}

void DynamicStream::dump() const { inform("%s\n", this->dumpString()); }