#include "dyn_stream.hh"
#include "stream.hh"
#include "stream_element.hh"
#include "stream_engine.hh"

#include "debug/StreamAlias.hh"
#include "debug/StreamBase.hh"
#include "debug/StreamCritical.hh"
#include "debug/StreamRangeSync.hh"
#define DEBUG_TYPE StreamBase
#include "stream_log.hh"

DynamicStream::DynamicStream(Stream *_stream,
                             const DynamicStreamId &_dynamicStreamId,
                             uint64_t _configSeqNum, Cycles _configCycle,
                             ThreadContext *_tc, StreamEngine *_se)
    : stream(_stream), dynamicStreamId(_dynamicStreamId),
      configSeqNum(_configSeqNum), configCycle(_configCycle), tc(_tc),
      FIFOIdx(_dynamicStreamId) {
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
  this->addValueBaseDynStreams();
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

void DynamicStream::addValueBaseDynStreams() {
  for (const auto &edge : this->stream->valueBaseEdges) {
    auto baseS = this->stream->se->getStream(edge.toStaticId);
    auto &baseDynS = baseS->getLastDynamicStream();
    DYN_S_DPRINTF(this->dynamicStreamId, "ValueBaseDynS %s Me %s.\n",
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
    this->valueBaseEdges.emplace_back(
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
         this->areNextBackBaseElementsAllocated() &&
         this->areNextValueBaseElementsAllocated();
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
                    "Released? Align(%llu), Reuse(%llu), BaseS %s\n",
                    this->FIFOIdx.entryIdx, baseElementIdx,
                    edge.alignBaseElement, edge.reuseBaseElement,
                    baseS->getStreamName());
      DYN_S_DPRINTF(this->dynamicStreamId, "BaseDynS %s.\n",
                    baseDynS.dumpString());
    }
    assert(baseElement && "Base Element Already Released?");
    if (baseElement->isStepped) {
      /**
       * This must be a misspeculated StreamStep, likely on the outer loop IV.
       * However, if I stop allocating here, the core may making progress
       * and never correct the misspeculation. This is observed in MinorCPU.
       * ! Hack: issue a warning for MinorCPU, but do not stop allocation.
       */
      if (this->stream->getCPUDelegator()->cpuType ==
          GemForgeCPUDelegator::CPUTypeE::MINOR) {
        DYN_S_WARN(this->dynamicStreamId,
                   "Ignore Misspeculatively Stepped AddrBaseElement, "
                   "NextElementIdx(%llu) BaseElementIdx(%llu)"
                   " Align(%llu), Reuse(%llu), BaseE %s\n",
                   this->FIFOIdx.entryIdx, baseElementIdx,
                   edge.alignBaseElement, edge.reuseBaseElement,
                   baseElement->FIFOIdx);
        continue;
      }
      DYN_S_DPRINTF(this->dynamicStreamId,
                    "NextElementIdx(%llu) BaseElementIdx(%llu) Already "
                    "(Misspeculatively) Stepped? Align(%llu), Reuse(%llu), "
                    "BaseE %s\n",
                    this->FIFOIdx.entryIdx, baseElementIdx,
                    edge.alignBaseElement, edge.reuseBaseElement,
                    baseElement->FIFOIdx);
      return false;
    }
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

bool DynamicStream::areNextValueBaseElementsAllocated() const {
  auto S = this->stream;
  for (const auto &edge : this->valueBaseEdges) {
    auto baseS = S->se->getStream(edge.baseStaticId);

    if (baseS == S) {
      /**
       * We don't allow self value dependence in valueBaseEdges.
       * Special cases like LoadComputeStream, UpdateStream are handled
       * in addValueBaseElements().
       */
      DYN_S_PANIC(this->dynamicStreamId, "ValueDependence on myself.");
    }

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
                    "(Misspeculatively) Stepped? Align(%llu), Reuse(%llu), "
                    "BaseStream %s\n",
                    this->FIFOIdx.entryIdx, baseElementIdx,
                    edge.alignBaseElement, edge.reuseBaseElement,
                    baseS->getStreamName());
      return false;
    }
  }
  return true;
}

void DynamicStream::addAddrBaseElements(StreamElement *newElement) {
  for (const auto &edge : this->addrBaseEdges) {
    this->addAddrBaseElementEdge(newElement, edge);
  }
}

void DynamicStream::addValueBaseElements(StreamElement *newElement) {

  auto S = this->stream;
  auto newElementIdx = newElement->FIFOIdx.entryIdx;
  for (const auto &edge : this->valueBaseEdges) {
    auto baseS = S->se->getStream(edge.baseStaticId);
    const auto &baseDynS =
        baseS->getDynamicStreamByInstance(edge.baseInstanceId);
    // Let's compute the base element entryIdx.
    uint64_t baseElementIdx = edge.alignBaseElement;
    if (edge.reuseBaseElement != 0) {
      baseElementIdx += newElementIdx / edge.reuseBaseElement;
    }
    // Try to find this element.
    auto baseElement = baseDynS.getElementByIdx(baseElementIdx);
    if (!baseElement) {
      S_ELEMENT_PANIC(newElement,
                      "Failed to find value base element %llu from %s.",
                      baseElementIdx, baseDynS.dynamicStreamId);
    }
    S_ELEMENT_DPRINTF(newElement, "Add ValueBaseElement: %s.\n",
                      baseElement->FIFOIdx);
    newElement->valueBaseElements.emplace_back(baseElement);
  }

  /**
   * LoadComputeStream/UpdateStream always has itself has the ValueBaseElement.
   */
  if (S->isLoadComputeStream() || S->isUpdateStream()) {
    S_ELEMENT_DPRINTF(newElement, "Add Self ValueBaseElement.\n");
    newElement->valueBaseElements.emplace_back(newElement);
  }

  if (newElementIdx == 0) {
    return;
  }

  /**
   * Add BackValueBaseElement.
   */
  for (const auto &edge : this->backBaseEdges) {
    auto baseS = S->se->getStream(edge.baseStaticId);
    const auto &baseDynS =
        baseS->getDynamicStreamByInstance(edge.baseInstanceId);
    // Let's compute the base element entryIdx.
    uint64_t baseElementIdx = edge.alignBaseElement;
    assert(edge.reuseBaseElement == 1 && "BackEdge should have reuse 1.");
    baseElementIdx += newElementIdx - 1;
    // Try to find this element.
    auto baseElement = baseDynS.getElementByIdx(baseElementIdx);
    if (!baseElement) {
      S_ELEMENT_PANIC(newElement,
                      "Failed to find back base element %llu from %s.",
                      baseElementIdx, baseDynS.dynamicStreamId);
    }
    S_ELEMENT_DPRINTF(newElement, "Add Back ValueBaseElement: %s.\n",
                      baseElement->FIFOIdx);
    newElement->valueBaseElements.emplace_back(baseElement);
  }
}

void DynamicStream::addAddrBaseElementEdge(StreamElement *newElement,
                                           const StreamDepEdge &edge) {
  auto S = this->stream;
  auto baseS = S->se->getStream(edge.baseStaticId);
  auto &baseDynS = baseS->getDynamicStreamByInstance(edge.baseInstanceId);
  // Let's compute the base element entryIdx.
  uint64_t baseElementIdx = edge.alignBaseElement;
  if (edge.reuseBaseElement != 0) {
    baseElementIdx += newElement->FIFOIdx.entryIdx / edge.reuseBaseElement;
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
  auto S = this->stream;
  if (!S->hasCoreUser() && this->offloadedToCache) {
    // Check that dependent dynS all offloaded to cache.
    for (auto depS : S->addrDepStreams) {
      const auto &depDynS = depS->getDynamicStream(this->configSeqNum);
      // If the AddrDepStream issues, then we have to issue to compute the
      // address.
      // if (depDynS.coreSENeedAddress()) {
      if (depDynS.shouldCoreSEIssue()) {
        return true;
      }
    }
    for (auto backDepS : S->backDepStreams) {
      const auto &backDepDynS = backDepS->getDynamicStream(this->configSeqNum);
      if (!backDepDynS.offloadedToCache) {
        return true;
      }
    }
    for (auto valDepS : S->valueDepStreams) {
      const auto &valDepDynS = valDepS->getDynamicStream(this->configSeqNum);
      if (!valDepDynS.offloadedToCache) {
        return true;
      }
    }
    /**
     * If we have some dependent nest stream region, we also have to issue.
     */
    if (S->hasDepNestRegion()) {
      return true;
    }
    return false;
  }
  /**
   * A special rule for conditional AtomicStream that already alias with a load.
   * This is common pattern to first load to check if we want to perform the
   * atomic. For such pattern, we do not issue.
   */
  if (!S->hasCoreUser() && S->isAtomicStream() && !S->isAtomicComputeStream() &&
      S->getIsConditional() && S->aliasBaseStream->aliasedStreams.size() > 1) {
    return false;
  }
  return true;
}

bool DynamicStream::coreSENeedAddress() const {
  if (!this->shouldCoreSEIssue()) {
    return false;
  }
  // A special case for AtomicComputeStream.
  if (this->offloadedToCache) {
    if (this->stream->isAtomicComputeStream()) {
      return false;
    }
  }
  return true;
}

bool DynamicStream::coreSEOracleValueReady() const {
  if (this->shouldCoreSEIssue()) {
    return false;
  }
  for (auto depS : this->stream->addrDepStreams) {
    const auto &depDynS = depS->getDynamicStream(this->configSeqNum);
    // If the AddrDepStream issues, then we have to issue to compute the
    // address.
    if (depDynS.coreSENeedAddress()) {
      return false;
    }
  }
  return true;
}

bool DynamicStream::shouldRangeSync() const {
  if (!this->stream->se->isStreamRangeSyncEnabled()) {
    return false;
  }
  if (!this->stream->isMemStream()) {
    return false;
  }
  if (!this->offloadedToCache) {
    return false;
  }
  if (this->stream->isAtomicComputeStream() || this->stream->isUpdateStream() ||
      this->stream->isStoreComputeStream()) {
    // Streams that writes to memory always require range-sync.
    return true;
  }
  for (auto depS : this->stream->addrDepStreams) {
    const auto &depDynS = depS->getDynamicStream(this->configSeqNum);
    if (depDynS.shouldRangeSync()) {
      return true;
    }
  }
  for (auto backDepS : this->stream->backDepStreams) {
    const auto &backDepDynS = backDepS->getDynamicStream(this->configSeqNum);
    if (backDepDynS.shouldRangeSync()) {
      return true;
    }
  }
  for (auto valDepS : this->stream->valueDepStreams) {
    const auto &valDepDynS = valDepS->getDynamicStream(this->configSeqNum);
    if (valDepDynS.shouldRangeSync()) {
      return true;
    }
  }
  // Finally for pure load stream, check if the core need the value.
  if (!this->shouldCoreSEIssue()) {
    // We are not issuing for this stream, should also do Range-Sync.
    return true;
  }
  return false;
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

StreamElement *DynamicStream::getFirstElement() { return this->tail->next; }
const StreamElement *DynamicStream::getFirstElement() const {
  return this->tail->next;
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

void DynamicStream::allocateElement(StreamElement *newElement) {

  auto S = this->stream;
  assert(S->isConfigured() &&
         "Stream should be configured to allocate element.");
  S->statistic.numAllocated++;
  newElement->stream = S;

  /**
   * Append this new element to the dynamic stream.
   */
  DYN_S_DPRINTF(this->dynamicStreamId, "Try to allocate element.\n");
  newElement->dynS = this;

  /**
   * next() is called after assign to make sure
   * entryIdx starts from 0.
   */
  newElement->FIFOIdx = this->FIFOIdx;
  newElement->isCacheBlockedValue = S->isMemStream();
  this->FIFOIdx.next();

  if (this->hasTotalTripCount() &&
      newElement->FIFOIdx.entryIdx >= this->getTotalTripCount() + 1) {
    DYN_S_PANIC(
        this->dynamicStreamId,
        "Allocate beyond totalTripCount %lu, allocSize %lu, entryIdx %lu.\n",
        this->getTotalTripCount(), S->getAllocSize(),
        newElement->FIFOIdx.entryIdx);
  }

  // Add addr/value base elements
  this->addAddrBaseElements(newElement);
  this->addValueBaseElements(newElement);

  newElement->allocateCycle = S->getCPUDelegator()->curCycle();

  // Append to the list.
  this->head->next = newElement;
  this->head = newElement;
  this->allocSize++;
  S->allocSize++;

  S_ELEMENT_DPRINTF(newElement, "Allocated.\n");

  /**
   * If there is no AddrBaseElement, we mark AddrReady immediately.
   * This is the case for most IV streams -- they will compute value
   * later.
   */
  if (newElement->addrBaseElements.empty()) {
    newElement->markAddrReady();
  }
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
                    releaseElement->isAddrReady());
  /**
   * Since this element is released as unstepped,
   * we need to reverse the FIFOIdx so that if we misspeculated,
   * new elements can be allocated with correct FIFOIdx.
   */
  this->FIFOIdx.prev();
  return releaseElement;
}

StreamElement *DynamicStream::releaseElementStepped(bool isEnd) {

  /**
   * This function performs a normal release, i.e. release a stepped
   * element from the stream.
   */

  assert(this->stepSize > 0 && "No element to release.");
  auto releaseElement = this->tail->next;
  assert(releaseElement->isStepped && "Release unstepped element.");

  const bool used = releaseElement->isFirstUserDispatched();
  bool late = false;

  auto S = this->stream;
  auto &statistic = S->statistic;

  statistic.numStepped++;
  if (used) {
    statistic.numUsed++;
    /**
     * Since this element is used by the core, we update the statistic
     * of the latency of this element experienced by the core.
     */
    if (releaseElement->valueReadyCycle <
        releaseElement->firstValueCheckCycle) {
      // The element is ready earlier than core's user.
      auto earlyCycles = releaseElement->firstValueCheckCycle -
                         releaseElement->valueReadyCycle;
      statistic.numCoreEarlyElement++;
      statistic.numCoreEarlyCycle += earlyCycles;
    } else {
      // The element makes the core's user wait.
      auto lateCycles = releaseElement->valueReadyCycle -
                        releaseElement->firstValueCheckCycle;
      statistic.numCoreLateElement++;
      statistic.numCoreLateCycle += lateCycles;
      late = true;
      if (lateCycles > 1000) {
        S_ELEMENT_DPRINTF_(
            StreamCritical, releaseElement,
            "Extreme Late %lu, Request Lat %lu, AddrReady %lu Issue %lu "
            "ValReady %lu FirstCheck %lu.\n",
            lateCycles,
            releaseElement->valueReadyCycle - releaseElement->issueCycle,
            releaseElement->addrReadyCycle - releaseElement->allocateCycle,
            releaseElement->issueCycle - releaseElement->allocateCycle,
            releaseElement->valueReadyCycle - releaseElement->allocateCycle,
            releaseElement->firstValueCheckCycle -
                releaseElement->allocateCycle);
      }
    }
  }
  this->updateStatsOnReleaseStepElement(S->getCPUDelegator()->curCycle(),
                                        releaseElement->addr, late);

  // Update the aliased statistic.
  if (releaseElement->isAddrAliased) {
    statistic.numAliased++;
  }
  if (releaseElement->flushed) {
    statistic.numFlushed++;
  }

  // Check if the element is faulted.
  if (S->isMemStream() && releaseElement->isAddrReady()) {
    if (releaseElement->isValueFaulted(releaseElement->addr,
                                       releaseElement->size)) {
      statistic.numFaulted++;
    }
  }

  /**
   * If this stream is not offloaded, and we have dispatched a StreamStore to
   * this element, we push the element to PendingWritebackElements.
   */
  if (!this->offloadedToCache && releaseElement->isFirstStoreDispatched()) {
    /**
     * So far only enable this for IndirectUpdateStream.
     */
    if (S->isUpdateStream() && S->isIndirectLoadStream()) {
      this->stream->se->addPendingWritebackElement(releaseElement);
    }
  }

  /**
   * Handle store func load stream.
   * This is tricky because the is not used by the core, we want
   * to distinguish the last element stepped by StreamEnd, which
   * should not perform computation.
   */
  if (!isEnd) {
    // this->handleMergedPredicate(dynS, releaseElement);
  }

  this->tail->next = releaseElement->next;
  if (this->stepped == releaseElement) {
    this->stepped = this->tail;
  }
  if (this->head == releaseElement) {
    this->head = this->tail;
  }
  this->stepSize--;
  this->allocSize--;
  S->allocSize--;

  S_ELEMENT_DPRINTF(releaseElement, "ReleaseElementStepped, used %d.\n", used);
  return releaseElement;
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

void DynamicStream::setTotalTripCount(int64_t totalTripCount) {
  if (this->hasTotalTripCount()) {
    DYN_S_PANIC(this->dynamicStreamId, "Reset TotalTripCount %lld -> %lld.",
                this->totalTripCount, totalTripCount);
  }
  DYN_S_DPRINTF(this->dynamicStreamId, "Set TotalTripCount %lld.\n",
                totalTripCount);
  this->totalTripCount = totalTripCount;
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

void DynamicStream::receiveStreamRange(
    const DynamicStreamAddressRangePtr &range) {
  if (!this->shouldRangeSync()) {
    DYN_S_PANIC(this->dynamicStreamId,
                "Receive StreamRange but RangeSync is not required.");
  }
  auto iter = this->receivedRanges.begin();
  auto end = this->receivedRanges.end();
  while (iter != end) {
    if ((*iter)->elementRange.getLHSElementIdx() >
        range->elementRange.getLHSElementIdx()) {
      break;
    }
    ++iter;
  }
  DYN_S_DPRINTF_(StreamRangeSync, this->dynamicStreamId,
                 "[Range] Receive StreamRange in core: %s.\n", *range);
  this->receivedRanges.insert(iter, range);
}

DynamicStreamAddressRangePtr DynamicStream::getNextReceivedRange() const {
  if (this->receivedRanges.empty()) {
    return nullptr;
  }
  return this->receivedRanges.front();
}

void DynamicStream::popReceivedRange() {
  assert(!this->receivedRanges.empty() && "Pop when there is no range.");
  this->receivedRanges.pop_front();
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
       << static_cast<int>(element->isAddrReady())
       << static_cast<int>(element->isValueReady) << ')';
    for (auto baseElement : element->addrBaseElements) {
      ss << '.' << baseElement->FIFOIdx.entryIdx;
    }
    ss << ' ';
  }
  return ss.str();
}

void DynamicStream::dump() const { inform("%s\n", this->dumpString()); }