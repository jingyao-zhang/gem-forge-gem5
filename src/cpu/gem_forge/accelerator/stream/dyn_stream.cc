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

const char *DynStream::StreamDepEdge::typeToString(const TypeE &type) {
#define Case(x)                                                                \
  case x:                                                                      \
    return #x
  switch (type) {
    Case(Addr);
    Case(Value);
    Case(Back);
#undef Case
  default:
    assert(false && "Invalid StreamDepEdgeType.");
  }
}

std::ostream &operator<<(std::ostream &os,
                         const DynStream::StreamDepEdge::TypeE &type) {
  os << DynStream::StreamDepEdge::typeToString(type);
  return os;
}

std::string to_string(const DynStream::StreamDepEdge::TypeE &type) {
  return std::string(DynStream::StreamDepEdge::typeToString(type));
}

DynStream::DynStream(Stream *_stream, const DynStreamId &_dynStreamId,
                     uint64_t _configSeqNum, Cycles _configCycle,
                     ThreadContext *_tc, StreamEngine *_se)
    : stream(_stream), se(_se), dynStreamId(_dynStreamId),
      configSeqNum(_configSeqNum), configCycle(_configCycle), tc(_tc),
      FIFOIdx(_dynStreamId) {
  this->tail = new StreamElement(_se);
  this->head = this->tail;
  this->stepped = this->tail;

  for (auto &hitPrivate : this->hitPrivateCacheHistory) {
    hitPrivate = false;
  }
  this->totalHitPrivateCache = 0;

  // Initialize the InnerLoopBaseStreamMap.
  for (const auto &edge : this->stream->innerLoopBaseEdges) {
    this->innerLoopBaseEdges.emplace(std::piecewise_construct,
                                     std::forward_as_tuple(edge.toStaticId),
                                     std::forward_as_tuple());
  }
}

DynStream::~DynStream() {
  delete this->tail;
  this->tail = nullptr;
  this->head = nullptr;
  this->stepped = nullptr;
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
    case StreamDepEdge::TypeE::Addr:
    case StreamDepEdge::TypeE::Value: {
      /**
       * The reuse count is delayed as StreamConfig has not executed,
       * and we don't know the trip count yet.
       * Simply initialize it to 1, as the most general case.
       */
      auto reuseBaseElement = 1;
      this->baseEdges.emplace_back(
          edge.type, edge.toStaticId, baseDynS.dynStreamId.streamInstance,
          edge.fromStaticId, alignBaseElementIdx, reuseBaseElement);
      break;
    }
    case StreamDepEdge::TypeE::Back: {
      /**
       * For back dependence, the reuse count should always be 1.
       */
      auto reuseBaseElement = 1;
      this->baseEdges.emplace_back(StreamDepEdge::TypeE::Back, edge.toStaticId,
                                   baseDynS.dynStreamId.streamInstance,
                                   edge.fromStaticId, alignBaseElementIdx,
                                   reuseBaseElement);
      baseDynS.backDepEdges.emplace_back(
          StreamDepEdge::TypeE::Back, edge.toStaticId,
          baseDynS.dynStreamId.streamInstance, edge.fromStaticId,
          alignBaseElementIdx, reuseBaseElement);
      break;
    }
    default: {
      DYN_S_PANIC(this->dynStreamId, "Illegal EdgeType %d.", edge.type);
    }
    }
  }

  /**
   * Register myself to OuterLoopDepStream.
   * Since StreamConfigure is dispatched in order, the user should be the last
   * DynStream at the moment.
   */
  for (const auto &edge : this->stream->innerLoopDepEdges) {
    auto depS = edge.toStream;
    auto &depDynS = depS->getLastDynStream();
    DYN_S_DPRINTF(this->dynStreamId,
                  "[InnerLoopDep] Push myself to OuterLoopDepDynS %s.\n",
                  depDynS.dynStreamId);
    depDynS.pushInnerLoopBaseDynStream(edge.type, edge.fromStaticId,
                                       this->dynStreamId.streamInstance,
                                       edge.toStaticId);
  }
}

int DynStream::getBaseElemReuseCount(Stream *baseS) const {
  for (const auto &edge : this->baseEdges) {
    if (this->se->getStream(edge.baseStaticId) == baseS) {
      return edge.reuseBaseElement;
    }
  }
  DYN_S_PANIC(this->dynStreamId, "Failed to find BaseStream %s.",
              baseS->getStreamName());
}

DynStream::StreamEdges &
DynStream::getInnerLoopBaseEdges(StaticId baseStaticId) {
  auto iter = this->innerLoopBaseEdges.find(baseStaticId);
  if (iter == this->innerLoopBaseEdges.end()) {
    DYN_S_PANIC(this->dynStreamId,
                "Failed to find InnerLoopBaseEdges for StaticId %lu.",
                baseStaticId);
  }
  return iter->second;
}

const DynStream::StreamEdges &
DynStream::getInnerLoopBaseEdges(StaticId baseStaticId) const {
  auto iter = this->innerLoopBaseEdges.find(baseStaticId);
  if (iter == this->innerLoopBaseEdges.end()) {
    DYN_S_PANIC(this->dynStreamId,
                "Failed to find InnerLoopBaseEdges for StaticId %lu.",
                baseStaticId);
  }
  return iter->second;
}

void DynStream::pushInnerLoopBaseDynStream(StreamDepEdge::TypeE type,
                                           StaticId baseStaticId,
                                           InstanceId baseInstanceId,
                                           StaticId depStaticId) {
  auto &edges = this->getInnerLoopBaseEdges(baseStaticId);
  edges.emplace_back(type, baseStaticId, baseInstanceId, depStaticId,
                     0 /* AlignBaseElementIdx */, 1 /* ReuseBaseElement */);
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
    if (edge.type != StreamDepEdge::TypeE::Addr &&
        edge.type != StreamDepEdge::TypeE::Value) {
      continue;
    }
    auto baseS = this->stream->se->getStream(edge.baseStaticId);
    auto &baseDynS = baseS->getDynStreamByInstance(edge.baseInstanceId);
    if (baseS->getLoopLevel() == this->stream->getLoopLevel()) {
      this->configureBaseDynStreamReuseSameLoop(edge, baseDynS);
    } else {
      this->configureBaseDynStreamReuseOuterLoop(edge, baseDynS);
    }
    DYN_S_DPRINTF(this->dynStreamId, "Configure Reuse %llu to Base %s.\n",
                  edge.reuseBaseElement, baseS->getStreamName());
  }
}

void DynStream::configureBaseDynStreamReuseSameLoop(StreamDepEdge &edge,
                                                    DynStream &baseDynS) {
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

void DynStream::configureBaseDynStreamReuseOuterLoop(StreamDepEdge &edge,
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
  edge.reuseBaseElement = reuse;
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
    case StreamDepEdge::TypeE::Addr: {
      if (!this->isNextAddrBaseElementAllocated(edge)) {
        return false;
      }
      break;
    }
    case StreamDepEdge::TypeE::Value: {
      if (!this->isNextValueBaseElementAllocated(edge)) {
        return false;
      }
      break;
    }
    case StreamDepEdge::TypeE::Back: {
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
    const StreamDepEdge &edge) const {
  auto S = this->stream;
  auto baseS = S->se->getStream(edge.baseStaticId);
  const auto &baseDynS = baseS->getDynStreamByInstance(edge.baseInstanceId);
  // Let's compute the base element entryIdx.
  uint64_t baseElementIdx = edge.alignBaseElement;
  if (edge.reuseBaseElement != 0) {
    baseElementIdx += this->FIFOIdx.entryIdx / edge.reuseBaseElement;
  }
  // Try to find this element.
  if (baseDynS.FIFOIdx.entryIdx <= baseElementIdx) {
    DYN_S_DPRINTF(this->dynStreamId,
                  "NextElementIdx(%llu) BaseElementIdx(%llu) Not Ready, "
                  "Align(%llu), Reuse(%llu), BaseStream %s.\n",
                  this->FIFOIdx.entryIdx, baseElementIdx, edge.alignBaseElement,
                  edge.reuseBaseElement, baseS->getStreamName());
    return false;
  }
  auto baseElement = baseDynS.getElemByIdx(baseElementIdx);
  if (!baseElement) {
    DYN_S_PANIC(this->dynStreamId,
                "NextElementIdx(%llu) BaseElementIdx(%llu) Already "
                "Released? Align(%llu), Reuse(%llu), BaseS %s\n BaseDynS %s",
                this->FIFOIdx.entryIdx, baseElementIdx, edge.alignBaseElement,
                edge.reuseBaseElement, baseS->getStreamName(),
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
                 this->FIFOIdx.entryIdx, baseElementIdx, edge.alignBaseElement,
                 edge.reuseBaseElement, baseElement->FIFOIdx);
      return true;
    }
    DYN_S_DPRINTF(this->dynStreamId,
                  "NextElementIdx(%llu) BaseElementIdx(%llu) Already "
                  "(Misspeculatively) Stepped? Align(%llu), Reuse(%llu), "
                  "BaseE %s\n",
                  this->FIFOIdx.entryIdx, baseElementIdx, edge.alignBaseElement,
                  edge.reuseBaseElement, baseElement->FIFOIdx);
    return false;
  }
  return true;
}

bool DynStream::isNextBackBaseElementAllocated(
    const StreamDepEdge &edge) const {
  auto S = this->stream;
  // No back dependence for the first element.
  if (this->FIFOIdx.entryIdx == 0) {
    return true;
  }
  auto baseS = S->se->getStream(edge.baseStaticId);
  const auto &baseDynS = baseS->getDynStreamByInstance(edge.baseInstanceId);
  // Let's compute the base element entryIdx.
  uint64_t baseElementIdx = edge.alignBaseElement;
  assert(edge.reuseBaseElement == 1 && "BackEdge should have reuse 1.");
  baseElementIdx += this->FIFOIdx.entryIdx - 1;
  // Try to find this element.
  if (baseDynS.FIFOIdx.entryIdx <= baseElementIdx) {
    DYN_S_DPRINTF(this->dynStreamId,
                  "NextElemIdx(%llu) BackBaseElemIdx(%llu) Not Ready, "
                  "Align(%llu), Reuse(%llu), BaseStream %s.\n",
                  this->FIFOIdx.entryIdx, baseElementIdx, edge.alignBaseElement,
                  edge.reuseBaseElement, baseS->getStreamName());
    return false;
  }
  auto baseElement = baseDynS.getElemByIdx(baseElementIdx);
  if (!baseElement) {
    DYN_S_PANIC(this->dynStreamId,
                "NextElemIdx(%llu) BackBaseElemIdx(%llu) Already "
                "Released? Align(%llu), Reuse(%llu), BaseDynS %s.",
                this->FIFOIdx.entryIdx, baseElementIdx, edge.alignBaseElement,
                edge.reuseBaseElement, baseDynS.dumpString());
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
    assert(edge.reuseBaseElement == 1 && "BackEdge should have reuse 1.");
    depElementIdx += element->FIFOIdx.entryIdx + 1;
    // Try to find this element.
    if (depDynS.FIFOIdx.entryIdx <= depElementIdx) {
      S_ELEMENT_DPRINTF(element,
                        "BackDepElemIdx(%llu) Not Allocated, Align(%llu), "
                        "Reuse(%llu), DepDynS %s.\n",
                        this->FIFOIdx.entryIdx, depElementIdx,
                        edge.alignBaseElement, edge.reuseBaseElement,
                        depDynS.dynStreamId);
      return false;
    }
    auto depElement = depDynS.getElemByIdx(depElementIdx);
    if (!depElement) {
      S_ELEMENT_PANIC(element,
                      "BackDepElemIdx(%llu) Already Released? Align(%llu), "
                      "Reuse(%llu), DepDynS %s %s.",
                      this->FIFOIdx.entryIdx, depElementIdx,
                      edge.alignBaseElement, edge.reuseBaseElement,
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
    const StreamDepEdge &edge) const {
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
  uint64_t baseElementIdx = edge.alignBaseElement;
  if (edge.reuseBaseElement != 0) {
    baseElementIdx += this->FIFOIdx.entryIdx / edge.reuseBaseElement;
  }
  // Try to find this element.
  if (baseDynS.FIFOIdx.entryIdx <= baseElementIdx) {
    DYN_S_DPRINTF(this->dynStreamId,
                  "NextElementIdx(%llu) BaseElementIdx(%llu) Not Ready, "
                  "Align(%llu), Reuse(%llu), BaseStream %s.\n",
                  this->FIFOIdx.entryIdx, baseElementIdx, edge.alignBaseElement,
                  edge.reuseBaseElement, baseS->getStreamName());
    return false;
  }
  auto baseElement = baseDynS.getElemByIdx(baseElementIdx);
  if (!baseElement) {
    DYN_S_DPRINTF(this->dynStreamId,
                  "NextElementIdx(%llu) BaseElementIdx(%llu) Already "
                  "Released? Align(%llu), Reuse(%llu), BaseStream %s\n",
                  this->FIFOIdx.entryIdx, baseElementIdx, edge.alignBaseElement,
                  edge.reuseBaseElement, baseS->getStreamName());
    DYN_S_DPRINTF(this->dynStreamId, "BaseDynS %s.\n", baseDynS.dumpString());
  }
  assert(baseElement && "Base Element Already Released?");
  if (baseElement->isStepped) {
    DYN_S_DPRINTF(this->dynStreamId,
                  "NextElementIdx(%llu) BaseElementIdx(%llu) Already "
                  "(Misspeculatively) Stepped? Align(%llu), Reuse(%llu), "
                  "BaseStream %s\n",
                  this->FIFOIdx.entryIdx, baseElementIdx, edge.alignBaseElement,
                  edge.reuseBaseElement, baseS->getStreamName());
    return false;
  }
  return true;
}

void DynStream::addBaseElements(StreamElement *newElement) {

  if (!this->shouldCoreSEIssue()) {
    return;
  }

  for (const auto &edge : this->baseEdges) {
    if (edge.type == StreamDepEdge::TypeE::Addr) {
      this->addAddrBaseElementEdge(newElement, edge);
    } else if (edge.type == StreamDepEdge::TypeE::Value) {
      this->addValueBaseElementEdge(newElement, edge);
    } else if (edge.type == StreamDepEdge::TypeE::Back) {
      this->addBackBaseElementEdge(newElement, edge);
    }
  }

  /**
   * LoadComputeStream/UpdateStream always has itself has the ValueBaseElement.
   */
  auto S = this->stream;
  if (S->isLoadComputeStream() || S->isUpdateStream()) {
    S_ELEMENT_DPRINTF(newElement, "Add Self ValueBaseElement.\n");
    newElement->valueBaseElements.emplace_back(newElement);
  }

  /**
   * Remember the InnerLoopBaseElement will be initialized later.
   */
  if (!this->stream->innerLoopBaseEdges.empty()) {
    newElement->hasUnInitInnerLoopAddrBaseElements = true;
  }
}

void DynStream::addAddrBaseElementEdge(StreamElement *newElement,
                                       const StreamDepEdge &edge) {
  auto S = this->stream;
  auto baseS = S->se->getStream(edge.baseStaticId);
  auto &baseDynS = baseS->getDynStreamByInstance(edge.baseInstanceId);
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
  auto baseElement = baseDynS.getElemByIdx(baseElementIdx);
  assert(baseElement && "Failed to find base element.");
  newElement->addrBaseElements.emplace_back(baseElement);
}

void DynStream::addValueBaseElementEdge(StreamElement *newElement,
                                        const StreamDepEdge &edge) {

  auto S = this->stream;
  auto newElementIdx = newElement->FIFOIdx.entryIdx;

  auto baseS = S->se->getStream(edge.baseStaticId);
  const auto &baseDynS = baseS->getDynStreamByInstance(edge.baseInstanceId);
  // Let's compute the base element entryIdx.
  uint64_t baseElementIdx = edge.alignBaseElement;
  if (edge.reuseBaseElement != 0) {
    baseElementIdx += newElementIdx / edge.reuseBaseElement;
  }
  // Try to find this element.
  auto baseElement = baseDynS.getElemByIdx(baseElementIdx);
  if (!baseElement) {
    S_ELEMENT_PANIC(newElement,
                    "Failed to find value base element %llu from %s.",
                    baseElementIdx, baseDynS.dynStreamId);
  }
  S_ELEMENT_DPRINTF(newElement, "Add ValueBaseElement: %s.\n",
                    baseElement->FIFOIdx);
  newElement->valueBaseElements.emplace_back(baseElement);
}

void DynStream::addBackBaseElementEdge(StreamElement *newElement,
                                       const StreamDepEdge &edge) {

  auto S = this->stream;
  auto newElementIdx = newElement->FIFOIdx.entryIdx;
  if (newElementIdx == 0) {
    // The first element has no BackBaseElement.
    return;
  }

  auto baseS = S->se->getStream(edge.baseStaticId);
  const auto &baseDynS = baseS->getDynStreamByInstance(edge.baseInstanceId);
  // Let's compute the base element entryIdx.
  uint64_t baseElementIdx = edge.alignBaseElement;
  assert(edge.reuseBaseElement == 1 && "BackEdge should have reuse 1.");
  baseElementIdx += newElementIdx - 1;
  // Try to find this element.
  auto baseElement = baseDynS.getElemByIdx(baseElementIdx);
  if (!baseElement) {
    S_ELEMENT_PANIC(newElement,
                    "Failed to find back base element %llu from %s.",
                    baseElementIdx, baseDynS.dynStreamId);
  }
  S_ELEMENT_DPRINTF(newElement, "Add Back ValueBaseElement: %s.\n",
                    baseElement->FIFOIdx);
  newElement->valueBaseElements.emplace_back(baseElement);
}

void DynStream::tryAddInnerLoopBaseElements(StreamElement *elem) {

  if (this->stream->innerLoopBaseEdges.empty() ||
      !elem->hasUnInitInnerLoopAddrBaseElements) {
    return;
  }

  if (!this->areInnerLoopBaseElementsAllocated(elem)) {
    return;
  }

  this->addInnerLoopBaseElements(elem);
  elem->hasUnInitInnerLoopAddrBaseElements = false;
}

bool DynStream::areInnerLoopBaseElementsAllocated(StreamElement *elem) const {

  if (this->stream->innerLoopBaseEdges.empty()) {
    return true;
  }

  auto elemIdx = elem->FIFOIdx.entryIdx;

  DYN_S_DPRINTF(this->dynStreamId,
                "[InnerLoopDep] Checking for NextElem %llu.\n", elemIdx);
  for (const auto &edge : this->stream->innerLoopBaseEdges) {
    auto baseS = edge.toStream;
    const auto &dynEdges = this->getInnerLoopBaseEdges(edge.toStaticId);
    if (elemIdx >= dynEdges.size()) {
      // The inner-loop stream has not been allocated.
      DYN_S_DPRINTF(this->dynStreamId,
                    "[InnerLoopDep]   InnerLoopS %s Not Config.\n",
                    baseS->getStreamName());
      return false;
    }
    const auto &dynEdge = dynEdges.at(elemIdx);
    auto &baseDynS = baseS->getDynStreamByInstance(dynEdge.baseInstanceId);
    DYN_S_DPRINTF(this->dynStreamId, "[InnerLoopDep]   InnerLoopDynS %s.\n",
                  baseDynS.dynStreamId);
    if (!baseDynS.hasTotalTripCount()) {
      DYN_S_DPRINTF(this->dynStreamId, "[InnerLoopDep]   NoTripCount.\n");
      return false;
    }
    auto baseTripCount = baseDynS.getTotalTripCount();
    if (baseDynS.FIFOIdx.entryIdx <= baseTripCount) {
      DYN_S_DPRINTF(
          this->dynStreamId,
          "[InnerLoopDep]   TripCount %llu > BaseDynS NextElem %lu.\n",
          baseTripCount, baseDynS.FIFOIdx.entryIdx);
      return false;
    }
    auto baseElement = baseDynS.getElemByIdx(baseTripCount);
    if (!baseElement) {
      DYN_S_PANIC(
          this->dynStreamId,
          "[InnerLoopDep]   InnerLoopDynS %s TripCount %llu ElemReleased.",
          baseDynS.dynStreamId, baseTripCount);
    }
  }

  return true;
}

void DynStream::addInnerLoopBaseElements(StreamElement *elem) {

  if (this->stream->innerLoopBaseEdges.empty()) {
    return;
  }

  auto elemIdx = elem->FIFOIdx.entryIdx;
  for (const auto &edge : this->stream->innerLoopBaseEdges) {
    auto baseS = edge.toStream;
    const auto &dynEdges = this->getInnerLoopBaseEdges(edge.toStaticId);
    if (elemIdx >= dynEdges.size()) {
      // The inner-loop stream has not been allocated.
      S_ELEMENT_PANIC(elem, "[InnerLoopDep] InnerLoopS %s Not Config.\n",
                      baseS->getStreamName());
    }
    const auto &dynEdge = dynEdges.at(elemIdx);
    auto &baseDynS = baseS->getDynStreamByInstance(dynEdge.baseInstanceId);
    if (!baseDynS.hasTotalTripCount()) {
      S_ELEMENT_PANIC(elem, "[InnerLoopDep] NoTripCount.\n");
    }
    auto baseTripCount = baseDynS.getTotalTripCount();
    if (baseDynS.FIFOIdx.entryIdx <= baseTripCount) {
      S_ELEMENT_PANIC(
          elem, "[InnerLoopDep] TripCount %llu > BaseDynS NextElem %lu.\n",
          baseTripCount, baseDynS.FIFOIdx.entryIdx);
    }
    assert(baseTripCount > 0 && "0 TripCount.");
    auto baseElemIdx = baseTripCount;
    if (baseS->isInnerSecondFinalValueUsedByCore()) {
      baseElemIdx = baseTripCount - 1;
    }
    auto baseElement = baseDynS.getElemByIdx(baseElemIdx);
    if (!baseElement) {
      S_ELEMENT_PANIC(
          elem, "[InnerLoopDep] %s TripCount %llu BaseElemIdx %llu Released.",
          baseDynS.dynStreamId, baseTripCount, baseElemIdx);
    }
    S_ELEMENT_DPRINTF(elem, "[InnerLoopDep] Add Type %s BaseElem: %s.\n",
                      edge.type, baseElement->FIFOIdx);
    switch (edge.type) {
    case StreamDepEdge::TypeE::Addr:
      elem->addrBaseElements.emplace_back(baseElement);
      break;
    default: {
      S_ELEMENT_DPRINTF(elem, "Unsupported InnerLoopDep %s.", edge.type);
    }
    }
  }
}

bool DynStream::shouldCoreSEIssue() const {
  /**
   * If the stream has floated, and no core user/dependent streams here,
   * then we don't have to issue for the data.
   * ZeroTripCount stream will never issue. This is common for EpilogueLoop when
   * the loop is perfectly vectorized.
   */
  auto S = this->stream;
  if (this->hasZeroTripCount()) {
    return false;
  }

  if (!S->hasCoreUser() && this->isFloatedToCache()) {
    // Check that dependent dynS all offloaded to cache (except ZeroTripCount).
    for (auto depS : S->addrDepStreams) {
      const auto &depDynS = depS->getDynStream(this->configSeqNum);
      // If the AddrDepStream issues, then we have to issue to compute the
      // address.
      if (depDynS.shouldCoreSEIssue()) {
        return true;
      }
    }
    for (auto backDepS : S->backDepStreams) {
      const auto &backDepDynS = backDepS->getDynStream(this->configSeqNum);
      if (!backDepDynS.isFloatedToCache()) {
        return true;
      }
    }
    for (auto valDepS : S->valueDepStreams) {
      const auto &valDepDynS = valDepS->getDynStream(this->configSeqNum);
      if (valDepDynS.shouldCoreSEIssue()) {
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

StreamElement *DynStream::getFirstElement() { return this->tail->next; }
const StreamElement *DynStream::getFirstElement() const {
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

void DynStream::allocateElement(StreamElement *newElement) {

  auto S = this->stream;
  assert(S->isConfigured() &&
         "Stream should be configured to allocate element.");
  S->statistic.numAllocated++;
  newElement->stream = S;

  /**
   * Append this new element to the dynamic stream.
   */
  DYN_S_DPRINTF(this->dynStreamId, "Try to allocate element.\n");
  newElement->dynS = this;

  /**
   * next() is called after assign to make sure
   * entryIdx starts from 0.
   */
  newElement->FIFOIdx = this->FIFOIdx;
  newElement->isCacheBlockedValue = S->isMemStream();
  this->FIFOIdx.next(this->stepElemCount);

  if (this->hasTotalTripCount() &&
      newElement->FIFOIdx.entryIdx >= this->getTotalTripCount() + 1) {
    DYN_S_PANIC(
        this->dynStreamId,
        "Allocate beyond totalTripCount %lu, allocSize %lu, entryIdx %lu.\n",
        this->getTotalTripCount(), S->getAllocSize(),
        newElement->FIFOIdx.entryIdx);
  }

  // Add base elements
  this->addBaseElements(newElement);

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
                  "NoUnsteppedElement config executed %d alloc %d stepped %d "
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
  if (auto elem = this->getFirstElement()) {
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

void DynStream::cancelFloat() {
  auto S = this->stream;
  S_DPRINTF(S, "Cancel FloatStream.\n");
  // We are no longer considered offloaded.
  this->setFloatedToCache(false);
  this->setFloatedToCacheAsRoot(false);
  this->floatPlan.clear();
  S->statistic.numFloatCancelled++;
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
  assert(this->isInnerLastElem(elemIdx) &&
         "SetFinalValue for Non-InnerLastElem.");
  if (this->innerFinalValueMap.count(elemIdx)) {
    DYN_S_PANIC(this->dynStreamId, "Reset InnerFinalValue at %lu.", elemIdx);
  }
  this->innerFinalValueMap.emplace(elemIdx, value);
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

std::string DynStream::dumpString() const {
  std::stringstream ss;
  ss << "===== " << this->dynStreamId << " total " << this->totalTripCount
     << " step " << this->stepSize << " alloc " << this->allocSize << " max "
     << this->stream->maxSize << " ========\n";
  auto element = this->tail;
  while (element != this->head) {
    element = element->next;
    ss << element->FIFOIdx.entryIdx << '('
       << static_cast<int>(element->isAddrReady())
       << static_cast<int>(element->isValueReady) << ')';
    ss << ' ';
  }
  return ss.str();
}

void DynStream::dump() const { inform("%s\n", this->dumpString()); }