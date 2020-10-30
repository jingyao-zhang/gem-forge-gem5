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

void DynamicStream::addAddrBaseDynStreams() {
  for (const auto &edge : this->stream->addrBaseEdges) {
    auto baseS = this->stream->se->getStream(edge.toStaticId);
    auto &baseDynS = baseS->getLastDynamicStream();
    DYN_S_DPRINTF(this->dynamicStreamId, "BaseDynS %s Me %s.\n",
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
                    "Align(%llu), Reuse"
                    "(%llu), BaseStream %s.\n",
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
    assert(baseElement && "Base Element Already Stepped?");
  }
  return true;
}

void DynamicStream::addAddrBaseElements(StreamElement *newElement) {
  for (const auto &edge : this->addrBaseEdges) {
    this->addAddrBaseElementEdge(newElement, edge);
  }
  this->addAddrBaseElementReduction(newElement);
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

void DynamicStream::addAddrBaseElementReduction(StreamElement *newElement) {
  auto S = this->stream;
  auto newElementIdx = newElement->FIFOIdx.entryIdx;
  /**
   * Find the previous element of my self for reduction stream.
   * However, if the reduction stream is offloaded without core user,
   * we do not try to track its BackMemBaseStream.
   */
  bool isOffloadedNoCoreUserReduction =
      this->offloadedToCache && !S->hasCoreUser() && S->isReduction();
  if (S->isReduction() && !isOffloadedNoCoreUserReduction) {
    if (newElementIdx > 0) {
      assert(this->head != this->tail &&
             "Failed to find previous element for reduction stream.");
      S_ELEMENT_DPRINTF(newElement, "Found reduction dependence.");
      newElement->addrBaseElements.insert(this->head);
    } else {
      // This is the first element. Let StreamElement::markAddrReady() set up
      // the initial value.
    }
  }
  if (newElementIdx > 0 && !isOffloadedNoCoreUserReduction) {
    // Find the back base element, starting from the second element.
    for (auto backBaseS : S->backBaseStreams) {
      if (backBaseS->getLoopLevel() != S->getLoopLevel()) {
        continue;
      }
      if (backBaseS->stepRootStream != nullptr) {
        // Try to find the previous element for the base.
        auto &baseDynS = backBaseS->getLastDynamicStream();
        auto baseElement = baseDynS.getElementByIdx(newElementIdx - 1);
        if (baseElement == nullptr) {
          S_ELEMENT_PANIC(newElement,
                          "Failed to find back base element from %s.\n",
                          backBaseS->getStreamName().c_str());
        }
        // ! Try to check the base element should have the previous element.
        S_ELEMENT_DPRINTF(baseElement, "Consumer for back dependence.\n");
        if (baseElement->FIFOIdx.streamId.streamInstance ==
            newElement->FIFOIdx.streamId.streamInstance) {
          S_ELEMENT_DPRINTF(newElement, "Found back dependence.\n");
          newElement->addrBaseElements.insert(baseElement);
        } else {
          // S_ELEMENT_PANIC(newElement,
          //                      "The base element has wrong
          //                      streamInstance.\n");
        }
      } else {
        // ! Should be a constant stream. So far we ignore it.
      }
    }
  }
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
  // We are no longer considered offloaded.
  this->offloadedToCache = false;
  this->offloadedToCacheAsRoot = false;
  S->statistic.numFloatCancelled++;
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