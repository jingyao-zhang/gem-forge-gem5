#include "dyn_stream.hh"
#include "stream.hh"
#include "stream_element.hh"
#include "stream_engine.hh"
#include "stream_region_controller.hh"

#include "debug/StreamAlias.hh"
#include "debug/StreamBase.hh"
#include "debug/StreamCritical.hh"
#include "debug/StreamRangeSync.hh"
#define DEBUG_TYPE StreamBase
#include "stream_log.hh"

namespace gem5 {

DynStream::DynStream(Stream *_stream, const DynStreamId &_dynStreamId,
                     uint64_t _configSeqNum, Cycles _configCycle,
                     ThreadContext *_tc, StreamEngine *_se)
    : stream(_stream), se(_se), dynStreamId(_dynStreamId),
      configSeqNum(_configSeqNum), configCycle(_configCycle), tc(_tc),
      FIFOIdx(_dynStreamId), innerLoopDepTracker(_dynStreamId) {
  this->tail = new StreamElement(_se);
  this->head = this->tail;
  this->stepped = this->tail;

  for (auto &hitPrivate : this->hitPrivateCacheHistory) {
    hitPrivate = false;
  }
  this->totalHitPrivateCache = 0;

  // Initialize the InnerLoopBaseStreamMap.
  for (const auto &edge : this->stream->innerLoopBaseEdges) {
    this->innerLoopDepTracker.trackAsInnerLoopBase(edge.toStaticId, edge.type);
  }
}

DynStream::~DynStream() {
  delete this->tail;
  this->tail = nullptr;
  this->head = nullptr;
  this->stepped = nullptr;
}

void DynStream::dispatchStreamEnd(uint64_t seqNum) {
  assert(!this->endDispatched && "DynS already ended.");
  this->endDispatched = true;
  this->endSeqNum = seqNum;
}

void DynStream::rewindStreamEnd() {
  this->endDispatched = false;
  this->endSeqNum = 0;
}

void DynStream::commitStreamEnd() {
  assert(this->configExecuted && "End before config executed.");
  assert(this->endDispatched && "End before end dispatched.");

  /**
   * We need to release all unstepped elements.
   */
  if (this->stepSize != 0) {
    DYN_S_PANIC(this->dynStreamId, "Commit StreamEnd with StepSize %d.",
                this->stepSize);
  }
  if (this->allocSize != 0) {
    DYN_S_PANIC(this->dynStreamId, "Commit StreamEnd with AllocSize %d.",
                this->allocSize);
  }

  // Update stats of cycles.
  auto endCycle = this->stream->getCPUDelegator()->curCycle();
  auto &statistic = this->stream->statistic;
  statistic.numCycle += endCycle - this->configCycle;

  if (this->hasTotalTripCount()) {
    statistic.numTotalTripCount += this->getTotalTripCount();
  } else if (this->FIFOIdx.entryIdx > 0) {
    // We have allocated something.
    statistic.numTotalTripCount += this->FIFOIdx.entryIdx - 1;
  }

  // Update float stats.
  if (this->isFloatedToCache()) {
    statistic.numFloated++;
    this->se->numFloated++;
    if (this->getFloatPlan().isFloatedToMem()) {
      statistic.numFloatMem++;
    }
    if (this->isPseudoFloatedToCache()) {
      statistic.numPseudoFloated++;
    }
  }

  // Remember the formal params history.
  this->stream->recordAggregateHistory(*this);
}

void DynStream::addBaseDynStreams() {

  for (const auto &edge : this->stream->baseEdges) {

    auto baseS = this->stream->se->getStream(edge.toStaticId);
    auto &baseDynS = baseS->getLastDynStream();
    DYN_S_DPRINTF(this->dynStreamId, "BaseEdge %s %s Me %s.\n", edge.type,
                  baseS->getStreamName(), this->dynStreamId.streamName);
    /**
     * If there are unstepped elements, we align to the first one.
     * If not (i.e. just created), we align to the next one.
     */
    auto alignBaseElementIdx = baseDynS.FIFOIdx.entryIdx;
    if (auto baseFirstUnsteppedElement = baseDynS.getFirstUnsteppedElem()) {
      alignBaseElementIdx = baseFirstUnsteppedElement->FIFOIdx.entryIdx;
    }

    switch (edge.type) {
    case DynStreamDepEdge::TypeE::Addr:
    case DynStreamDepEdge::TypeE::Value:
    case DynStreamDepEdge::TypeE::Pred: {
      /**
       * The reuse count is delayed as StreamConfig has not executed,
       * and we don't know the trip count yet.
       * Simply initialize it to 1, as the most general case.
       */
      auto reuseBaseElement = 1;
      this->baseEdges.emplace_back(edge.type, baseS, edge.toStaticId,
                                   baseDynS.dynStreamId.streamInstance,
                                   edge.fromStaticId, alignBaseElementIdx,
                                   reuseBaseElement);
      break;
    }
    case DynStreamDepEdge::TypeE::Back: {
      /**
       * For back dependence, the reuse count should always be 1.
       */
      auto reuseBaseElement = 1;
      this->baseEdges.emplace_back(
          DynStreamDepEdge::TypeE::Back, baseS, edge.toStaticId,
          baseDynS.dynStreamId.streamInstance, edge.fromStaticId,
          alignBaseElementIdx, reuseBaseElement);
      baseDynS.backDepEdges.emplace_back(
          DynStreamDepEdge::TypeE::Back, baseS, edge.toStaticId,
          baseDynS.dynStreamId.streamInstance, edge.fromStaticId,
          alignBaseElementIdx, reuseBaseElement);
      break;
    }
    default: {
      DYN_S_PANIC(this->dynStreamId, "Illegal EdgeType %s.", edge.type);
    }
    }
  }
}

void DynStream::addOuterDepDynStreams(StreamEngine *outerSE,
                                      InstSeqNum outerSeqNum) {

  /**
   * OuterLoopBoundDep needs some special handling.
   * So far this only works with NestInnerStream.
   */
  auto &baseTracker = this->innerLoopDepTracker;
  auto addOuterDepDynLoopBound =
      [this, outerSE, outerSeqNum,
       &baseTracker](const Stream::StreamDepEdge &edge) -> void {
    assert(outerSE && outerSeqNum != InvalidInstSeqNum);
    auto &depDynRegion =
        outerSE->regionController->getDynRegion("OuterLoopBound", outerSeqNum);
    auto &depDynLoopBound = depDynRegion.loopBound;
    auto &depTracker = depDynLoopBound.innerLoopDepTracker;
    DYN_S_DPRINTF(this->dynStreamId,
                  "[InnerLoopDep] Push myself to OuterLoopBound %s.\n",
                  depDynRegion.staticRegion->region.region());

    baseTracker.pushInnerLoopDepS(outerSE, nullptr, depTracker.dynId);
    depTracker.pushInnerLoopBaseDynStream(
        edge.type, this->stream, edge.fromStaticId,
        this->dynStreamId.streamInstance, edge.toStaticId);
  };

  /**
   * Register myself to OuterLoopDep Stream or LoopBound.
   * If we specified the outerSeqNum (from nest stream), we use that DynRegion.
   * Otherwise, we just get the last DynRegion.
   */
  for (const auto &edge : this->stream->innerLoopDepEdges) {
    if (edge.type == DynStreamDepEdge::TypeE::Bound) {
      addOuterDepDynLoopBound(edge);
      continue;
    }
    StreamEngine *depSE = nullptr;
    Stream *depS = nullptr;
    DynStream *depDynS = nullptr;
    if (outerSE != nullptr && outerSeqNum != InvalidInstSeqNum) {
      depSE = outerSE;
      depS = outerSE->getStream(edge.toStaticId);
      depDynS = &depS->getDynStream(outerSeqNum);
    } else {
      depSE = this->se;
      depS = edge.toStream;
      depDynS = &depS->getLastDynStream();
    }
    DYN_S_DPRINTF(this->dynStreamId,
                  "[InnerLoopDep] Push myself to OuterLoopDepDynS %s.\n",
                  depDynS->dynStreamId);
    baseTracker.pushInnerLoopDepS(depSE, depS, depDynS->dynStreamId);
    depDynS->innerLoopDepTracker.pushInnerLoopBaseDynStream(
        edge.type, this->stream, edge.fromStaticId,
        this->dynStreamId.streamInstance, edge.toStaticId);
  }
}

int DynStream::getBaseElemReuseCount(Stream *baseS) const {
  for (const auto &edge : this->baseEdges) {
    if (this->se->getStream(edge.baseStaticId) == baseS) {
      return edge.baseElemReuseCnt;
    }
  }
  DYN_S_PANIC(this->dynStreamId, "Failed to find BaseStream %s.",
              baseS->getStreamName());
}

int DynStream::getBaseElemSkipCount(Stream *baseS) const {
  for (const auto &edge : this->baseEdges) {
    if (this->se->getStream(edge.baseStaticId) == baseS) {
      return edge.baseElemSkipCnt;
    }
  }
  DYN_S_PANIC(this->dynStreamId, "Failed to find BaseStream %s.",
              baseS->getStreamName());
}

void DynStream::addStepStreams() {
  if (this->stream->isStepRoot()) {
    const auto &stepStreams = this->stream->se->getStepStreamList(this->stream);
    for (auto &stepS : stepStreams) {
      auto &stepDynS =
          stepS->getDynStreamByInstance(this->dynStreamId.streamInstance);
      this->stepDynStreams.push_back(&stepDynS);
    }
  }
}

void DynStream::configureBaseDynStreamReuse() {
  for (auto &edge : this->baseEdges) {
    if (edge.type != DynStreamDepEdge::TypeE::Addr &&
        edge.type != DynStreamDepEdge::TypeE::Value) {
      continue;
    }
    auto baseS = this->stream->se->getStream(edge.baseStaticId);
    auto &baseDynS = baseS->getDynStreamByInstance(edge.baseInstanceId);
    if (baseS->getLoopLevel() == this->stream->getLoopLevel()) {
      this->configureBaseDynStreamReuseSameLoop(edge, baseDynS);
    } else if (baseS->getLoopLevel() < this->stream->getLoopLevel()) {
      this->configureBaseDynStreamReuseOuterLoop(edge, baseDynS);
    } else {
      this->configureBaseDynStreamSkipInnerLoop(edge, baseDynS);
    }
    DYN_S_DPRINTF(this->dynStreamId, "Configure Reuse %llu to Base %s.\n",
                  edge.baseElemReuseCnt, baseS->getStreamName());
  }
}

void DynStream::configureBaseDynStreamReuseSameLoop(DynStreamDepEdge &edge,
                                                    DynStream &baseDynS) {
  auto baseS = baseDynS.stream;
  auto S = this->stream;
  if (baseS->stepRootStream == S->stepRootStream) {
    // We are synchronized by the same step root.
    edge.baseElemReuseCnt = 1;
  } else {
    // The other one must be a constant stream.
    // Reuse 0 means always reuse.
    assert(baseS->stepRootStream == nullptr && "Should be a constant stream.");
    edge.baseElemReuseCnt = 0;
  }
}

void DynStream::configureBaseDynStreamReuseOuterLoop(DynStreamDepEdge &edge,
                                                     DynStream &baseDynS) {
  auto baseS = baseDynS.stream;
  auto S = this->stream;
  auto baseLoopLevel = baseS->getLoopLevel();
  auto loopLevel = S->getLoopLevel();
  assert(baseLoopLevel < loopLevel && "Base should be outer.");
  assert(S->stepRootStream && "Miss StepRoot for the DepS.");
  auto &stepRootDynS = S->stepRootStream->getDynStream(this->configSeqNum);

  auto stepRootAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
      stepRootDynS.addrGenCallback);
  assert(stepRootAddrGen && "Should be linear addr gen.");
  auto reuse = stepRootAddrGen->getNestTripCount(
      stepRootDynS.addrGenFormalParams, loopLevel - baseLoopLevel);
  DYN_S_DPRINTF(this->dynStreamId,
                "OuterBaseS %s Reuse %ld Between LoopLevel %d-%d.\n",
                baseDynS.dynStreamId, reuse, baseLoopLevel, loopLevel);
  if (reuse == 0) {
    /**
     * This is possible when the inner loop is from some vectorized loop,
     * and the number of iterations are smaller than vectorized width --
     * all handled by the loop epilogure.
     * Sanity check that my TripCount is zero.
     */
    if (!this->hasTotalTripCount() || this->getTotalTripCount() != 0) {
      DYN_S_PANIC(this->dynStreamId,
                  "OuterBaseS %s ZeroReuse but NonZero TripCount %ld.",
                  baseDynS.dynStreamId, this->getTotalTripCount());
    }
  }
  edge.baseElemReuseCnt = reuse;
}

void DynStream::configureBaseDynStreamSkipInnerLoop(DynStreamDepEdge &edge,
                                                    DynStream &baseDynS) {
  auto baseS = baseDynS.stream;
  auto S = this->stream;
  auto baseLoopLevel = baseS->getLoopLevel();
  auto loopLevel = S->getLoopLevel();
  assert(baseLoopLevel > loopLevel && "Base should be inner.");

  assert(this->hasTotalTripCount());
  assert(baseDynS.hasTotalTripCount());
  auto depTripCount = this->getTotalTripCount();
  auto baseTripCount = baseDynS.getTotalTripCount();
  assert(depTripCount < baseTripCount);
  assert(baseTripCount % depTripCount == 0);

  auto skip = baseTripCount / depTripCount;

  DYN_S_DPRINTF(this->dynStreamId,
                "InnerBaseS %s Skip %ld Between LoopLevel %d-%d.\n",
                baseDynS.dynStreamId, skip, loopLevel, baseLoopLevel);
  if (skip == 0) {
    /**
     * This is possible when the inner loop is from some vectorized loop,
     * and the number of iterations are smaller than vectorized width --
     * all handled by the loop epilogure.
     * Sanity check that my TripCount is zero.
     */
    if (depTripCount != 0) {
      DYN_S_PANIC(this->dynStreamId,
                  "InnerBaseS %s ZeroSkip but NonZero TripCount %ld.",
                  baseDynS.dynStreamId, this->getTotalTripCount());
    }
  }
  edge.baseElemSkipCnt = skip;
}

bool DynStream::areNextBaseElementsAllocated() const {

  /**
   * If we are not going to issue, i.e. floated and no core usage,
   * we don't check for BaseElement dep.
   */
  if (!this->shouldCoreSEIssue()) {
    return true;
  }

  for (const auto &edge : this->baseEdges) {
    switch (edge.type) {
    case DynStreamDepEdge::TypeE::Addr: {
      if (!this->isNextAddrBaseElementAllocated(edge)) {
        return false;
      }
      break;
    }
    case DynStreamDepEdge::TypeE::Value: {
      if (!this->isNextValueBaseElementAllocated(edge)) {
        return false;
      }
      break;
    }
    case DynStreamDepEdge::TypeE::Back: {
      if (!this->isNextBackBaseElementAllocated(edge)) {
        return false;
      }
      break;
    }
    default: {
      DYN_S_PANIC(this->dynStreamId, "Illegal StreamDepEdge type %d.",
                  edge.type);
    }
    }
  }
  return true;
}

bool DynStream::isNextAddrBaseElementAllocated(
    const DynStreamDepEdge &edge) const {
  auto S = this->stream;
  auto baseS = S->se->getStream(edge.baseStaticId);
  const auto &baseDynS = baseS->getDynStreamByInstance(edge.baseInstanceId);
  // Let's compute the base element entryIdx.
  assert(edge.baseElemSkipCnt == 0 && "AddrEdge should have skip 0.");
  auto baseElemIdx = edge.getBaseElemIdx(this->FIFOIdx.entryIdx);
  // Try to find this element.
  if (baseDynS.FIFOIdx.entryIdx <= baseElemIdx) {
    DYN_S_DPRINTF(this->dynStreamId,
                  "NextElem(%llu) BaseElem(%llu) Not Ready, Align(%llu), "
                  "Reuse(%llu), BaseS %s.\n",
                  this->FIFOIdx.entryIdx, baseElemIdx, edge.alignBaseElement,
                  edge.baseElemReuseCnt, baseS->getStreamName());
    return false;
  }
  auto baseElement = baseDynS.getElemByIdx(baseElemIdx);
  if (!baseElement) {
    DYN_S_PANIC(this->dynStreamId,
                "NextElementIdx(%llu) BaseElementIdx(%llu) Already "
                "Released? Align(%llu), Reuse(%llu), BaseS %s\n BaseDynS %s",
                this->FIFOIdx.entryIdx, baseElemIdx, edge.alignBaseElement,
                edge.baseElemReuseCnt, baseS->getStreamName(),
                baseDynS.dumpString());
  }
  if (baseElement->isStepped) {
    /**
     * This must be a misspeculated StreamStep, likely on the outer loop IV.
     * However, if I stop allocating here, the core may making progress
     * and never correct the misspeculation. This is observed in MinorCPU.
     * ! Hack: issue a warning for MinorCPU, but do not stop allocation.
     */
    if (this->stream->getCPUDelegator()->cpuType ==
        GemForgeCPUDelegator::CPUTypeE::MINOR) {
      DYN_S_WARN(this->dynStreamId,
                 "Ignore Misspeculatively Stepped AddrBaseElement, "
                 "NextElementIdx(%llu) BaseElementIdx(%llu)"
                 " Align(%llu), Reuse(%llu), BaseE %s\n",
                 this->FIFOIdx.entryIdx, baseElemIdx, edge.alignBaseElement,
                 edge.baseElemReuseCnt, baseElement->FIFOIdx);
      return true;
    }
    DYN_S_DPRINTF(this->dynStreamId,
                  "NextElementIdx(%llu) BaseElementIdx(%llu) Already "
                  "(Misspeculatively) Stepped? Align(%llu), Reuse(%llu), "
                  "BaseE %s\n",
                  this->FIFOIdx.entryIdx, baseElemIdx, edge.alignBaseElement,
                  edge.baseElemReuseCnt, baseElement->FIFOIdx);
    return false;
  }
  return true;
}

bool DynStream::isNextBackBaseElementAllocated(
    const DynStreamDepEdge &edge) const {
  auto S = this->stream;
  // No back dependence for the first element.
  if (this->FIFOIdx.entryIdx == 0) {
    return true;
  }
  auto baseS = S->se->getStream(edge.baseStaticId);
  const auto &baseDynS = baseS->getDynStreamByInstance(edge.baseInstanceId);
  // Let's compute the base element entryIdx.
  assert(edge.baseElemReuseCnt == 1 && "BackEdge should have reuse 1.");
  assert(edge.baseElemSkipCnt == 0 && "BackEdge should have skip 0.");
  auto baseElemIdx = edge.getBaseElemIdx(this->FIFOIdx.entryIdx) - 1;
  // Try to find this element.
  if (baseDynS.FIFOIdx.entryIdx <= baseElemIdx) {
    DYN_S_DPRINTF(this->dynStreamId,
                  "NextElem %llu BackBaseElem %llu Not Ready, "
                  "Align %llu Reuse %llu BaseS %s.\n",
                  this->FIFOIdx.entryIdx, baseElemIdx, edge.alignBaseElement,
                  edge.baseElemReuseCnt, baseS->getStreamName());
    return false;
  }
  auto baseElement = baseDynS.getElemByIdx(baseElemIdx);
  if (!baseElement) {
    DYN_S_PANIC(this->dynStreamId,
                "NextElemIdx %llu BackBaseElemIdx %llu Already "
                "Released? Align %llu Reuse %llu BaseDynS %s.",
                this->FIFOIdx.entryIdx, baseElemIdx, edge.alignBaseElement,
                edge.baseElemReuseCnt, baseDynS.dynStreamId);
  }
  return true;
}

bool DynStream::areNextBackDepElementsReady(StreamElement *element) const {
  auto S = this->stream;
  for (const auto &edge : this->backDepEdges) {
    auto depS = S->se->getStream(edge.depStaticId);
    const auto &depDynS = depS->getDynStreamByInstance(edge.baseInstanceId);

    if (!depDynS.shouldCoreSEIssue()) {
      // If the DepDynS won't issue, we don't bother checking this.
      continue;
    }

    // Let's compute the base element entryIdx.
    uint64_t depElementIdx = edge.alignBaseElement;
    assert(edge.baseElemReuseCnt == 1 && "BackEdge should have reuse 1.");
    assert(edge.baseElemSkipCnt == 0 && "BackEdge should have skip 0.");
    depElementIdx += element->FIFOIdx.entryIdx + 1;
    // Try to find this element.
    if (depDynS.FIFOIdx.entryIdx <= depElementIdx) {
      S_ELEMENT_DPRINTF(element,
                        "BackDepElemIdx(%llu) Not Allocated, Align(%llu), "
                        "Reuse(%llu), DepDynS %s.\n",
                        this->FIFOIdx.entryIdx, depElementIdx,
                        edge.alignBaseElement, edge.baseElemReuseCnt,
                        depDynS.dynStreamId);
      return false;
    }
    auto depElement = depDynS.getElemByIdx(depElementIdx);
    if (!depElement) {
      S_ELEMENT_PANIC(element,
                      "BackDepElemIdx(%llu) Already Released? Align(%llu), "
                      "Reuse(%llu), DepDynS %s %s.",
                      this->FIFOIdx.entryIdx, depElementIdx,
                      edge.alignBaseElement, edge.baseElemReuseCnt,
                      depDynS.dynStreamId, depDynS.dumpString());
    }
    if (depElement->isElemFloatedToCache()) {
      // No need to enforce for floated elements.
    } else {
      if (!depElement->isValueReady) {
        S_ELEMENT_DPRINTF(element, "UnReady BackDepElement %s.\n",
                          depElement->FIFOIdx);
        return false;
      }
    }
  }
  return true;
}

bool DynStream::isNextValueBaseElementAllocated(
    const DynStreamDepEdge &edge) const {
  auto S = this->stream;
  auto baseS = S->se->getStream(edge.baseStaticId);

  if (baseS == S) {
    /**
     * We don't allow self value dependence in value base edges.
     * Special cases like LoadComputeStream, UpdateStream are handled
     * in addBaseElements().
     */
    DYN_S_PANIC(this->dynStreamId, "ValueDependence on myself.");
  }

  const auto &baseDynS = baseS->getDynStreamByInstance(edge.baseInstanceId);
  // Let's compute the base element entryIdx.
  auto baseElemIdx = edge.getBaseElemIdx(this->FIFOIdx.entryIdx);
  // Try to find this element.
  if (baseDynS.FIFOIdx.entryIdx <= baseElemIdx) {
    DYN_S_DPRINTF(this->dynStreamId,
                  "NextElem(%llu) BaseElem(%llu) Not Ready, Align %llu, Reuse "
                  "%llu, Skip %llu BaseS %s.\n",
                  this->FIFOIdx.entryIdx, baseElemIdx, edge.alignBaseElement,
                  edge.baseElemReuseCnt, edge.baseElemSkipCnt,
                  baseS->getStreamName());
    return false;
  }
  auto baseElement = baseDynS.getElemByIdx(baseElemIdx);
  if (!baseElement) {
    DYN_S_DPRINTF(this->dynStreamId,
                  "NextElementIdx(%llu) BaseElementIdx(%llu) Already "
                  "Released? Align(%llu), Reuse(%llu), BaseStream %s\n",
                  this->FIFOIdx.entryIdx, baseElemIdx, edge.alignBaseElement,
                  edge.baseElemReuseCnt, baseS->getStreamName());
    DYN_S_DPRINTF(this->dynStreamId, "BaseDynS %s.\n", baseDynS.dumpString());
  }
  assert(baseElement && "Base Element Already Released?");
  if (baseElement->isStepped) {
    DYN_S_DPRINTF(this->dynStreamId,
                  "NextElementIdx(%llu) BaseElementIdx(%llu) Already "
                  "(Misspeculatively) Stepped? Align(%llu), Reuse(%llu), "
                  "BaseStream %s\n",
                  this->FIFOIdx.entryIdx, baseElemIdx, edge.alignBaseElement,
                  edge.baseElemReuseCnt, baseS->getStreamName());
    return false;
  }
  return true;
}

void DynStream::addBaseElements(StreamElement *newElem) {

  if (!this->shouldCoreSEIssue() && newElem->isFloatElem()) {
    return;
  }

  for (const auto &edge : this->baseEdges) {
    if (edge.type == DynStreamDepEdge::TypeE::Addr) {
      this->addAddrBaseElementEdge(newElem, edge);
    } else if (edge.type == DynStreamDepEdge::TypeE::Value) {
      this->addValueBaseElementEdge(newElem, edge);
    } else if (edge.type == DynStreamDepEdge::TypeE::Back) {
      this->addBackBaseElementEdge(newElem, edge);
    }
  }

  /**
   * LoadComputeStream/UpdateStream always has itself has the ValueBaseElement.
   */
  auto S = this->stream;
  if (S->isLoadComputeStream() || S->isUpdateStream()) {
    S_ELEMENT_DPRINTF(newElem, "Add Self ValueBaseElement.\n");
    newElem->valueBaseElements.emplace_back(newElem);
  }

  /**
   * Remember the InnerLoopBaseElement will be initialized later.
   */
  for (const auto &innerBaseEdge : this->stream->innerLoopBaseEdges) {
    switch (innerBaseEdge.type) {
    case DynStreamDepEdge::TypeE::Addr: {
      newElem->hasUnInitInnerLoopAddrBaseElem = true;
      break;
    }
    case DynStreamDepEdge::TypeE::Back: {
      if (newElem->FIFOIdx.entryIdx > 0) {
        newElem->hasUnInitInnerLoopValueBaseElem = true;
      }
      break;
    }
    case DynStreamDepEdge::TypeE::Value: {
      newElem->hasUnInitInnerLoopValueBaseElem = true;
      break;
    }
    default: {
      S_ELEMENT_PANIC(newElem, "Unhandled InnerLoopBaseEdge %s.",
                      innerBaseEdge.toStream->getStreamName());
    }
    }
  }
}

void DynStream::addAddrBaseElementEdge(StreamElement *newElem,
                                       const DynStreamDepEdge &edge) {
  auto S = this->stream;
  auto baseS = S->se->getStream(edge.baseStaticId);
  auto &baseDynS = baseS->getDynStreamByInstance(edge.baseInstanceId);
  // Let's compute the base element entryIdx.
  auto baseElemIdx = edge.getBaseElemIdx(newElem->FIFOIdx.entryIdx);
  S_ELEMENT_DPRINTF(
      newElem,
      "BaseElementIdx(%llu), Align(%llu), Reuse (%llu), BaseStream %s.\n",
      baseElemIdx, edge.alignBaseElement, edge.baseElemReuseCnt,
      baseS->getStreamName());
  // Try to find this element.
  auto baseElem = baseDynS.getElemByIdx(baseElemIdx);
  assert(baseElem && "Failed to find base element.");
  newElem->addrBaseElements.emplace_back(baseElem);
}

void DynStream::addValueBaseElementEdge(StreamElement *newElem,
                                        const DynStreamDepEdge &edge) {

  auto S = this->stream;
  auto newElemIdx = newElem->FIFOIdx.entryIdx;

  auto baseS = S->se->getStream(edge.baseStaticId);
  const auto &baseDynS = baseS->getDynStreamByInstance(edge.baseInstanceId);
  // Let's compute the base element entryIdx.
  auto baseElemIdx = edge.getBaseElemIdx(newElemIdx);
  // Try to find this element.
  auto baseElem = baseDynS.getElemByIdx(baseElemIdx);
  if (!baseElem) {
    S_ELEMENT_PANIC(newElem, "Failed to find value base element %llu from %s.",
                    baseElemIdx, baseDynS.dynStreamId);
  }
  S_ELEMENT_DPRINTF(newElem, "Add ValueBaseElement: %s.\n", baseElem->FIFOIdx);
  newElem->valueBaseElements.emplace_back(baseElem);
}

void DynStream::addBackBaseElementEdge(StreamElement *newElem,
                                       const DynStreamDepEdge &edge) {

  auto S = this->stream;
  auto newElementIdx = newElem->FIFOIdx.entryIdx;
  if (newElementIdx == 0) {
    // The first element has no BackBaseElement.
    return;
  }

  auto baseS = S->se->getStream(edge.baseStaticId);
  const auto &baseDynS = baseS->getDynStreamByInstance(edge.baseInstanceId);
  // Let's compute the base element entryIdx.
  uint64_t baseElemIdx = edge.alignBaseElement;
  assert(edge.baseElemReuseCnt == 1 && "BackEdge should have reuse 1.");
  assert(edge.baseElemSkipCnt == 0 && "BackEdge should have skip 0.");
  baseElemIdx += newElementIdx - 1;
  // Try to find this element.
  auto baseElem = baseDynS.getElemByIdx(baseElemIdx);
  if (!baseElem) {
    S_ELEMENT_PANIC(newElem, "Failed to find BackBaseElem %llu from %s.",
                    baseElemIdx, baseDynS.dynStreamId);
  }
  S_ELEMENT_DPRINTF(newElem, "Add Back ValueBaseElem: %s.\n",
                    baseElem->FIFOIdx);
  newElem->valueBaseElements.emplace_back(baseElem);
}

void DynStream::tryAddInnerLoopBaseElements(StreamElement *elem) {

  if (this->stream->innerLoopBaseEdges.empty() ||
      !(elem->hasUnInitInnerLoopAddrBaseElem ||
        elem->hasUnInitInnerLoopValueBaseElem)) {
    return;
  }

  if (!this->innerLoopDepTracker.areInnerLoopBaseElemsAllocated(
          elem->FIFOIdx)) {
    return;
  }

  this->addInnerLoopBaseElements(elem);
  elem->hasUnInitInnerLoopAddrBaseElem = false;
  elem->hasUnInitInnerLoopValueBaseElem = false;
}

void DynStream::addInnerLoopBaseElements(StreamElement *elem) {

  if (this->stream->innerLoopBaseEdges.empty()) {
    return;
  }

  StreamInnerLoopDepTracker::BaseStreamElemVec baseElems;
  this->innerLoopDepTracker.getInnerLoopBaseElems(elem->FIFOIdx, baseElems);

  for (const auto &baseE : baseElems) {
    auto baseType = baseE.first;
    auto baseElem = baseE.second;
    switch (baseType) {
    case DynStreamDepEdge::TypeE::Addr: {
      elem->addrBaseElements.emplace_back(baseElem);
      baseElem->innerLoopDepElements.emplace_back(elem);
      break;
    }
    case DynStreamDepEdge::TypeE::Back:
    case DynStreamDepEdge::TypeE::Value: {
      elem->valueBaseElements.emplace_back(baseElem);
      baseElem->innerLoopDepElements.emplace_back(elem);
      break;
    }
    default: {
      S_ELEMENT_DPRINTF(elem, "Unsupported InnerLoopDep %s.", baseType);
    }
    }
  }
}

bool DynStream::shouldCoreSEIssue() const {
  /**
   * If the stream has floated, and no core user/dependent streams here,
   * then we don't have to issue for the data.
   * ZeroTripCount stream will never issue. This is common for EpilogueLoop
   * when the loop is perfectly vectorized.
   */
  auto S = this->stream;
  if (this->hasZeroTripCount()) {
    return false;
  }

  if (!S->hasCoreUser() && this->isFloatedToCache()) {
    // Check that dependent dynS all offloaded to cache (except
    // ZeroTripCount).
    for (auto depS : S->addrDepStreams) {

      if (depS->getLoopLevel() < S->getConfigLoopLevel()) {
        // I am an InnerLoopBaseS. Do not check.
        continue;
      }

      const auto &depDynS = depS->getDynStream(this->configSeqNum);
      // If the AddrDepStream issues, then we have to issue to compute the
      // address.
      if (depDynS.shouldCoreSEIssue()) {
        DYN_S_DPRINTF(this->dynStreamId,
                      "Should CoreSE Issue Due to AddrDepS %s.\n",
                      depDynS.dynStreamId);
        return true;
      }
    }
    for (auto backDepS : S->backDepStreams) {

      if (backDepS->getLoopLevel() < S->getConfigLoopLevel()) {
        // I am an InnerLoopBaseS. Do not check.
        continue;
      }

      const auto &backDepDynS = backDepS->getDynStream(this->configSeqNum);
      if (!backDepDynS.isFloatedToCache()) {
        DYN_S_DPRINTF(this->dynStreamId,
                      "Should CoreSE Issue Due to BackDepS %s.\n",
                      backDepDynS.dynStreamId);
        return true;
      }
    }
    for (auto valDepS : S->valueDepStreams) {

      if (valDepS->getLoopLevel() < S->getConfigLoopLevel()) {
        // I am an InnerLoopBaseS. Do not check.
        continue;
      }

      const auto &valDepDynS = valDepS->getDynStream(this->configSeqNum);
      if (valDepDynS.shouldCoreSEIssue()) {
        DYN_S_DPRINTF(this->dynStreamId,
                      "Should CoreSE Issue Due to ValDepS %s.\n",
                      valDepDynS.dynStreamId);
        return true;
      }
    }
    /**
     * If we have some dependent nest stream region, we also have to issue.
     */
    if (S->hasDepNestRegion()) {
      DYN_S_DPRINTF(this->dynStreamId,
                    "Should CoreSE Issue Due to DepNestRegion.\n");
      return true;
    }
    return false;
  }
  /**
   * A special rule for conditional AtomicStream that already alias with a
   * load. This is common pattern to first load to check if we want to perform
   * the atomic. For such pattern, we do not issue.
   */
  if (!S->hasCoreUser() && S->isAtomicStream() && !S->isAtomicComputeStream() &&
      S->getIsConditional() && S->aliasBaseStream->aliasedStreams.size() > 1) {
    return false;
  }
  DYN_S_DPRINTF(this->dynStreamId, "Should CoreSE Issue isFloated %d.\n",
                this->isFloatedToCache());
  return true;
}

bool DynStream::coreSENeedAddress() const {
  if (!this->shouldCoreSEIssue()) {
    return false;
  }
  // A special case for AtomicComputeStream.
  if (this->isFloatedToCache()) {
    if (this->stream->isAtomicComputeStream()) {
      return false;
    }
  }
  return true;
}

bool DynStream::coreSEOracleValueReady() const {
  if (this->shouldCoreSEIssue()) {
    return false;
  }
  for (auto depS : this->stream->addrDepStreams) {
    const auto &depDynS = depS->getDynStream(this->configSeqNum);
    // If the AddrDepStream issues, then we have to issue to compute the
    // address.
    if (depDynS.coreSENeedAddress()) {
      return false;
    }
  }
  return true;
}

bool DynStream::shouldRangeSync() const {
  if (!this->stream->se->isStreamRangeSyncEnabled()) {
    return false;
  }
  if (!this->stream->isMemStream()) {
    return false;
  }
  if (!this->isFloatedToCache()) {
    return false;
  }
  if (this->stream->isAtomicComputeStream() || this->stream->isUpdateStream() ||
      this->stream->isStoreComputeStream()) {
    /**
     * Streams that writes to memory always require range-sync.
     */
    return true;
  }
  for (auto depS : this->stream->addrDepStreams) {
    const auto &depDynS = depS->getDynStream(this->configSeqNum);
    if (depDynS.shouldRangeSync()) {
      return true;
    }
  }
  for (auto backDepS : this->stream->backDepStreams) {
    const auto &backDepDynS = backDepS->getDynStream(this->configSeqNum);
    if (backDepDynS.shouldRangeSync()) {
      return true;
    }
  }
  for (auto valDepS : this->stream->valueDepStreams) {
    const auto &valDepDynS = valDepS->getDynStream(this->configSeqNum);
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

uint64_t DynStream::getFirstFloatElemIdxOfStepGroup() const {
  if (this->isFloatedToCache()) {
    return this->getFirstFloatElemIdx();
  }
  for (const auto &stepDynS : this->stepDynStreams) {
    if (stepDynS->isFloatedToCache()) {
      return stepDynS->getFirstFloatElemIdx();
    }
  }
  DYN_S_PANIC(this->dynStreamId, "StepGroup not Floated.");
}

StreamElement *DynStream::getElemByIdx(uint64_t elementIdx) const {
  for (auto element = this->tail->next; element != nullptr;
       element = element->next) {
    if (element->FIFOIdx.entryIdx == elementIdx) {
      return element;
    }
  }
  return nullptr;
}

StreamElement *DynStream::getPrevElement(StreamElement *element) {
  assert(element->FIFOIdx.streamId == this->dynStreamId &&
         "Element is not mine.");
  for (auto prevElement = this->tail; prevElement != nullptr;
       prevElement = prevElement->next) {
    if (prevElement->next == element) {
      return prevElement;
    }
  }
  assert(false && "Failed to find the previous element.");
}

StreamElement *DynStream::getFirstElem() { return this->tail->next; }
const StreamElement *DynStream::getFirstElem() const {
  return this->tail->next;
}

StreamElement *DynStream::getFirstUnsteppedElem() const {
  if (this->allocSize <= this->stepSize) {
    return nullptr;
  }
  auto element = this->stepped->next;
  // * Notice the element is guaranteed to be not stepped.
  assert(!element->isStepped && "Dispatch user to stepped stream element.");
  return element;
}

StreamElement *DynStream::stepElement(bool isEnd) {
  auto element = this->getFirstUnsteppedElem();
  S_ELEMENT_DPRINTF(element, "Stepped by Stream%s.\n",
                    (isEnd ? "End" : "Step"));
  element->isStepped = true;
  this->stepped = element;
  this->stepSize++;
  return element;
}

StreamElement *DynStream::unstepElement() {
  assert(this->stepSize > 0 && "No element to unstep.");
  auto element = this->stepped;
  assert(element->isStepped && "Element not stepped.");
  element->isStepped = false;
  // Search to get previous element.
  this->stepped = this->getPrevElement(element);
  this->stepSize--;
  return element;
}

void DynStream::allocateElement(StreamElement *newElem) {

  auto S = this->stream;
  assert(S->isConfigured() &&
         "Stream should be configured to allocate element.");
  S->statistic.numAllocated++;
  newElem->stream = S;

  /**
   * Append this new element to the dynamic stream.
   */
  DYN_S_DPRINTF(this->dynStreamId, "Try to allocate element.\n");
  newElem->dynS = this;

  /**
   * next() is called after assign to make sure
   * entryIdx starts from 0.
   */
  newElem->FIFOIdx = this->FIFOIdx;
  newElem->isCacheBlockedValue = S->isMemStream();
  this->FIFOIdx.next(this->stepElemCount);

  /**
   * Check and memorize if the elem is floated to cache.
   */
  newElem->checkIsElemFloatedToCache();

  if (this->hasTotalTripCount() &&
      newElem->FIFOIdx.entryIdx >=
          this->getTotalTripCount() + this->stepElemCount) {
    DYN_S_PANIC(this->dynStreamId,
                "Allocate beyond tripCnt %lu stepCnt %ld, allocSize %lu, "
                "entryIdx %lu.\n",
                this->getTotalTripCount(), this->stepElemCount,
                S->getAllocSize(), newElem->FIFOIdx.entryIdx);
  }

  // Set remote bank if we have.
  {
    auto iter = this->futureElemBanks.find(newElem->FIFOIdx.entryIdx);
    if (iter != this->futureElemBanks.end()) {
      newElem->setRemoteBank(iter->second);
      this->futureElemBanks.erase(iter);
    }
  }

  // Add base elements
  this->addBaseElements(newElem);

  newElem->allocateCycle = S->getCPUDelegator()->curCycle();

  // Append to the list.
  this->head->next = newElem;
  this->head = newElem;
  this->allocSize++;
  S->allocSize++;

  S_ELEMENT_DPRINTF(newElem, "Allocated.\n");

  /**
   * If there is no AddrBaseElement, we mark AddrReady immediately.
   * This is the case for most IV streams -- they will compute value
   * later.
   */
  if (newElem->addrBaseElements.empty()) {
    if (!this->stream->isMemStream()) {
      // IV stream.
      newElem->markAddrReady();
    } else {
      // Mem stream. Make sure we are not issuing.
      if (this->shouldCoreSEIssue()) {
        newElem->markAddrReady();
      }
    }
  }
}

StreamElement *DynStream::releaseElementUnstepped() {
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
  this->FIFOIdx.prev(this->stepElemCount);
  return releaseElement;
}

bool DynStream::hasUnsteppedElem() const {
  auto element = this->getFirstUnsteppedElem();
  if (!element) {
    // We don't have element for this used stream.
    DYN_S_DPRINTF(this->dynStreamId,
                  "NoUnsteppedElem config executed %d alloc %d stepped %d "
                  "total %d next %s.\n",
                  this->configExecuted, this->allocSize, this->stepSize,
                  this->getTotalTripCount(), this->FIFOIdx);
    return false;
  }
  return true;
}

bool DynStream::isElemStepped(uint64_t elemIdx) const {
  if (auto elem = this->getFirstUnsteppedElem()) {
    return elemIdx < elem->FIFOIdx.entryIdx;
  } else {
    return elemIdx < this->FIFOIdx.entryIdx;
  }
}

bool DynStream::isElemReleased(uint64_t elemIdx) const {
  if (auto elem = this->getFirstElem()) {
    return elemIdx < elem->FIFOIdx.entryIdx;
  } else {
    return elemIdx < this->FIFOIdx.entryIdx;
  }
}

StreamElement *DynStream::releaseElementStepped(bool isEnd) {

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
  if (!releaseElement->isElemFloatedToCache() &&
      releaseElement->isFirstStoreDispatched()) {
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

void DynStream::updateStatsOnReleaseStepElement(Cycles releaseCycle,
                                                uint64_t vaddr, bool late) {
  this->numReleaseElement++;
  if (this->numReleaseElement == 1) {
    this->startVAddr = vaddr;
  }
  if (late) {
    this->lateElementCount++;
  }
  if (this->numReleaseElement % DynStream::HistoryWindowSize == 0) {
    if (this->numReleaseElement >= 3 * DynStream::HistoryWindowSize) {
      // Time to update.
      this->avgTurnAroundCycle =
          Cycles((releaseCycle - this->lastReleaseCycle) /
                 DynStream::HistoryWindowSize);
      this->numLateElement = this->lateElementCount;
    }
    // Time to reset.
    this->lastReleaseCycle = releaseCycle;
    this->lateElementCount = 0;
  }
}

void DynStream::recordHitHistory(bool hitPrivateCache) {
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
  if (this->isFloatedToCacheAsRoot()) {
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

void DynStream::tryCancelFloat() {
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
  if (this->isFloatedWithDependent()) {
    return;
  }
  // Try cancel the whole coalesce group.
  auto sid = S->getCoalesceBaseStreamId();
  std::vector<DynStream *> endStreams;
  if (sid == 0) {
    // No coalesce group, just cancel myself.
    endStreams.push_back(this);
  } else {
    auto coalesceBaseS = S->se->getStream(sid);
    const auto &coalesceGroupS = coalesceBaseS->coalesceGroupStreams;
    for (auto &coalescedS : coalesceGroupS) {
      auto &dynS = coalescedS->getDynStream(this->configSeqNum);
      // Simply check that it has been floated.
      if (dynS.isFloatedToCacheAsRoot() && !dynS.isFloatedWithDependent()) {
        endStreams.push_back(&dynS);
      }
    }
  }
  // Construct the end ids.
  if (endStreams.empty()) {
    return;
  }
  std::vector<DynStreamId> endIds;
  for (auto endDynS : endStreams) {
    endIds.push_back(endDynS->dynStreamId);
    endDynS->cancelFloat();
  }
  S->se->sendStreamFloatEndPacket(endIds);
}

void DynStream::updateFloatInfoForElems() {
  // Update all StreamElement info.
  for (auto elem = this->tail->next; elem; elem = elem->next) {
    elem->checkIsElemFloatedToCache();
  }
}

void DynStream::cancelFloat() {
  auto S = this->stream;
  S_DPRINTF(S, "Cancel FloatStream.\n");
  // We are no longer considered offloaded.
  this->setFloatedToCache(false);
  this->setFloatedToCacheAsRoot(false);
  this->floatPlan.clear();
  S->statistic.numFloatCancelled++;
  this->updateFloatInfoForElems();
}

bool DynStream::isInnerSecondElem(uint64_t elemIdx) const {
  assert(this->configExecuted && "The DynS has not be configured.");
  if (!this->hasInnerTripCount()) {
    return false;
  }
  if (elemIdx == 0) {
    return false;
  }
  return ((elemIdx - 1) % this->getInnerTripCount()) == 0;
}

bool DynStream::isInnerLastElem(uint64_t elemIdx) const {
  assert(this->configExecuted && "The DynS has not be configured.");
  if (!this->hasInnerTripCount()) {
    return false;
  }
  if (elemIdx == 0) {
    return false;
  }
  return (elemIdx % this->getInnerTripCount()) == 0;
}

bool DynStream::isInnerSecondLastElem(uint64_t elemIdx) const {
  assert(this->configExecuted && "The DynS has not be configured.");
  if (!this->hasInnerTripCount()) {
    return false;
  }
  return ((elemIdx + 1) % this->getInnerTripCount()) == 0;
}

void DynStream::setInnerFinalValue(uint64_t elemIdx, const StreamValue &value) {
  if (!this->isInnerLastElem(elemIdx)) {
    DYN_S_PANIC(this->dynStreamId, "SetFinalValue for Non-InnerLastElem %lu.",
                elemIdx);
  }
  if (this->innerFinalValueMap.count(elemIdx)) {
    DYN_S_PANIC(this->dynStreamId, "Reset InnerFinalValue at %lu.", elemIdx);
  }
  DYN_S_DPRINTF(this->dynStreamId,
                "[InnerLoopDep] Set InnerFinalValue at %lu %s.\n", elemIdx,
                value);
  this->innerFinalValueMap.emplace(elemIdx, value);
  // We should add ourselves back to the IssuingList.
  this->se->addIssuingDynS(this);
}

bool DynStream::isInnerFinalValueReady(uint64_t elemIdx) const {
  assert(this->isInnerLastElem(elemIdx) &&
         "SetFinalValue for Non-InnerLastElem.");
  return this->innerFinalValueMap.count(elemIdx);
}

const StreamValue &DynStream::getInnerFinalValue(uint64_t elemIdx) const {
  assert(this->isInnerFinalValueReady(elemIdx));
  return this->innerFinalValueMap.at(elemIdx);
}

void DynStream::setTotalAndInnerTripCount(int64_t tripCount) {
  if (this->hasTotalTripCount() && tripCount != this->totalTripCount) {
    DYN_S_PANIC(this->dynStreamId, "Reset TotalTripCount %lld -> %lld.",
                this->totalTripCount, tripCount);
  }
  DYN_S_DPRINTF(this->dynStreamId, "Set Total/InnerTripCount %lld.\n",
                tripCount);
  this->totalTripCount = tripCount;
  this->innerTripCount = tripCount;
}

void DynStream::setInnerTripCount(int64_t innerTripCount) {
  if (this->innerTripCount != this->totalTripCount) {
    DYN_S_PANIC(this->dynStreamId, "Reset InnerTripCount %lld -> %lld.",
                this->innerTripCount, innerTripCount);
  }
  DYN_S_DPRINTF(this->dynStreamId, "Set InnerTripCount %lld.\n",
                innerTripCount);
  this->innerTripCount = innerTripCount;
}

int32_t DynStream::getBytesPerMemElement() const {
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

void DynStream::receiveStreamRange(const DynStreamAddressRangePtr &range) {
  if (!this->shouldRangeSync()) {
    DYN_S_PANIC(this->dynStreamId,
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
  DYN_S_DPRINTF_(StreamRangeSync, this->dynStreamId,
                 "[Range] Receive StreamRange in core: %s.\n", *range);
  this->receivedRanges.insert(iter, range);
}

DynStreamAddressRangePtr DynStream::getNextReceivedRange() const {
  if (this->receivedRanges.empty()) {
    return nullptr;
  }
  return this->receivedRanges.front();
}

void DynStream::popReceivedRange() {
  assert(!this->receivedRanges.empty() && "Pop when there is no range.");
  this->receivedRanges.pop_front();
}

bool DynStream::isLoopElimInCoreStoreCmpS() const {
  return this->stream->isStoreComputeStream() &&
         this->stream->isLoopEliminated() && !this->isFloatedToCache();
}

void DynStream::setElementRemoteBank(uint64_t elemIdx, int remoteBank) {
  if (elemIdx >= this->FIFOIdx.entryIdx) {
    // This is a future element bank.
    this->futureElemBanks.emplace(elemIdx, remoteBank);
    return;
  }
  if (auto elem = this->getElemByIdx(elemIdx)) {
    elem->setRemoteBank(remoteBank);
    return;
  }
  DYN_S_PANIC(this->dynStreamId, "No Elem %lu to set RemoteBank.", elemIdx);
}

std::string DynStream::dumpString() const {
  std::stringstream ss;
  ss << "===== ";
  ss << '-' << this->dynStreamId.coreId;
  ss << '-' << this->dynStreamId.staticId;
  ss << '-' << this->dynStreamId.streamInstance;
  ss << '-';
  ss << " total " << this->totalTripCount;
  ss << " ack " << this->cacheAckedElements.size();
  ss << " step " << this->stepSize;
  ss << " alloc " << this->allocSize;
  ss << " max " << this->stream->maxSize;
  ss << " next " << this->FIFOIdx.entryIdx;
  if (this->stream->isInnerFinalValueUsedByCore() && this->isFloatedToCache()) {
    ss << " final-elem " << this->innerFinalValueMap.size();
  }
  if (this->shouldCoreSEIssue()) {
    ss << " issuing " << this->se->isDynSIssuing(const_cast<DynStream *>(this));
  }
  ss << " ========";
  if (this->getNumInnerLoopDepS() != 0) {
    ss << '\n';
    ss << "-- InnerLoopDepS";
    for (const auto &depId : this->getInnerLoopDepS()) {
      ss << ' ' << depId.dynId;
    }
  }
  auto elem = this->tail;

  auto dumpEdge = [&](const StreamElement::ElementEdge &baseEdge) -> void {
    ss << ' ';
    ss << baseEdge.getIdx();
    ss << ' ';
    ss << 'V';
    ss << baseEdge.isValid();
  };

  while (elem != this->head) {
    elem = elem->next;
    ss << '\n';
    ss << "  ";
    ss << elem->FIFOIdx.entryIdx;
    ss << '(';
    ss << elem->isAddrReady();
    ss << elem->isValueReady;
    if (elem->shouldIssue() && this->stream->isMemStream()) {
      ss << elem->isReqIssued();
    }
    ss << ')';
    ss << " AddrBaseE";
    for (const auto &baseEdge : elem->addrBaseElements) {
      dumpEdge(baseEdge);
    }
    ss << " ValBaseE";
    for (const auto &baseEdge : elem->valueBaseElements) {
      dumpEdge(baseEdge);
    }
  }

  return ss.str();
}

void DynStream::dump() const {
  // I don't want the file location.
  NO_LOC_INFORM("%s\n", this->dumpString());
}
} // namespace gem5
