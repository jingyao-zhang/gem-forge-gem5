#include "LLCDynamicStream.hh"
#include "LLCStreamEngine.hh"

#include "cpu/gem_forge/accelerator/stream/stream.hh"
#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "debug/LLCRubyStreamBase.hh"
#include "debug/LLCRubyStreamLife.hh"
#include "debug/LLCRubyStreamReduce.hh"
#include "debug/LLCRubyStreamStore.hh"
#define DEBUG_TYPE LLCRubyStreamBase
#include "../stream_log.hh"

std::unordered_map<DynamicStreamId, LLCDynamicStream *, DynamicStreamIdHasher>
    LLCDynamicStream::GlobalLLCDynamicStreamMap;
std::unordered_map<NodeID, std::list<std::vector<LLCDynamicStream *>>>
    LLCDynamicStream::GlobalMLCToLLCDynamicStreamGroupMap;

// TODO: Support real flow control.
LLCDynamicStream::LLCDynamicStream(
    AbstractStreamAwareController *_mlcController,
    AbstractStreamAwareController *_llcController,
    CacheStreamConfigureDataPtr _configData)
    : mlcController(_mlcController), llcController(_llcController),
      configData(_configData),
      slicedStream(_configData, true /* coalesceContinuousElements */),
      configureCycle(_mlcController->curCycle()), sliceIdx(0),
      allocatedSliceIdx(_configData->initAllocatedIdx) {

  // Remember the SendTo configs.
  for (auto &depEdge : this->configData->depEdges) {
    if (depEdge.type == CacheStreamConfigureData::DepEdge::Type::SendTo) {
      this->sendToConfigs.emplace_back(depEdge.data);
    }
  }

  // Remember the BaseOn configs.
  for (auto &baseEdge : this->configData->baseEdges) {
    this->baseOnConfigs.emplace_back(baseEdge.data.lock());
    assert(this->baseOnConfigs.back() && "BaseStreamConfig already released?");
  }

  if (this->configData->isPointerChase) {
    // Pointer chase stream can only have at most one base requests waiting for
    // data.
    assert(false && "PointerChase is not supported for now.");
  }
  if (this->getStaticStream()->isReduction()) {
    // Initialize the first element for ReductionStream with the initial value.
    assert(this->isOneIterationBehind() &&
           "ReductionStream must be OneIterationBehind.");
    this->nextElementIdx = 1;
  }
  assert(GlobalLLCDynamicStreamMap.emplace(this->getDynamicStreamId(), this)
             .second);
  this->sanityCheckStreamLife();
}

LLCDynamicStream::~LLCDynamicStream() {
  for (auto &indirectStream : this->indirectStreams) {
    delete indirectStream;
    indirectStream = nullptr;
  }
  this->indirectStreams.clear();
  assert(GlobalLLCDynamicStreamMap.erase(this->getDynamicStreamId()) == 1);
}

bool LLCDynamicStream::hasTotalTripCount() const {
  if (this->baseStream) {
    return this->baseStream->hasTotalTripCount();
  }
  return this->configData->totalTripCount != -1;
}

uint64_t LLCDynamicStream::getTotalTripCount() const {
  if (this->baseStream) {
    return this->baseStream->getTotalTripCount();
  }
  return this->configData->totalTripCount;
}

Addr LLCDynamicStream::peekVAddr() const {
  return this->slicedStream.peekNextSlice().vaddr;
}

const DynamicStreamSliceId &LLCDynamicStream::peekSlice() const {
  return this->slicedStream.peekNextSlice();
}

Addr LLCDynamicStream::getVAddr(uint64_t sliceIdx) const {
  panic("getVAddr is deprecated.\n");
  return 0;
}

bool LLCDynamicStream::translateToPAddr(Addr vaddr, Addr &paddr) const {
  // ! Do something reasonable here to translate the vaddr.
  auto cpuDelegator = this->configData->stream->getCPUDelegator();
  return cpuDelegator->translateVAddrOracle(vaddr, paddr);
}

void LLCDynamicStream::addCredit(uint64_t n) {
  this->allocatedSliceIdx += n;
  for (auto indirectStream : this->indirectStreams) {
    indirectStream->addCredit(n);
  }
}

void LLCDynamicStream::updateIssueClearCycle() {
  if (!this->shouldUpdateIssueClearCycle()) {
    return;
  }
  // ! Hack: enforce issue clear cycle to 10 if we have sendTo.
  // if (!this->sendToConfigs.empty()) {
  //   // this->issueClearCycle = Cycles(20);
  //   return;
  // }
  const auto *dynS =
      this->configData->stream->getDynamicStream(this->configData->dynamicId);
  if (dynS == nullptr) {
    // The dynS is already released.
    return;
  }
  auto avgTurnAroundCycle = dynS->getAvgTurnAroundCycle();
  auto avgLateElements = dynS->getNumLateElement();
  uint64_t newIssueClearCycle = this->issueClearCycle;
  if (avgTurnAroundCycle != 0) {
    // We need to adjust the turn around cycle from element to slice.
    uint64_t avgSliceTurnAroundCycle = static_cast<float>(avgTurnAroundCycle) *
                                       this->slicedStream.getElementPerSlice();
    // We divide by 1.5 so to reflect that we should be slighly faster than
    // core.
    uint64_t adjustSliceTurnAroundCycle = avgSliceTurnAroundCycle * 2 / 3;
    if (avgLateElements >= 2) {
      // If we have late elements, try to issue faster.
      newIssueClearCycle = std::max(1ul, this->issueClearCycle / 2);
    } else {
      newIssueClearCycle = adjustSliceTurnAroundCycle;
    }
    if (newIssueClearCycle != this->issueClearCycle) {
      /**
       * Some the stats from the core may be disruptly. we have some simple
       * threshold here.
       * TODO: Improve this.
       */
      const uint64_t IssueClearThreshold = 1024;
      LLC_S_DPRINTF(this->configData->dynamicId,
                    "Update IssueClearCycle %lu -> %lu (%lu), avgEleTurn %lu, "
                    "avgSliceTurn %lu, avgLateEle %d, elementPerSlice %f.\n",
                    this->issueClearCycle, newIssueClearCycle,
                    IssueClearThreshold, avgTurnAroundCycle,
                    avgSliceTurnAroundCycle, avgLateElements,
                    this->slicedStream.getElementPerSlice());
      this->issueClearCycle =
          Cycles(newIssueClearCycle > IssueClearThreshold ? IssueClearThreshold
                                                          : newIssueClearCycle);
    }
  }
}

bool LLCDynamicStream::shouldUpdateIssueClearCycle() {
  if (!this->shouldUpdateIssueClearCycleInitialized) {
    // We do not constrain ourselves from the core if there are no core users
    // for both myself and all the indirect streams.
    this->shouldUpdateIssueClearCycleMemorized = true;
    if (!this->getStaticStream()->hasCoreUser()) {
      bool hasCoreUser = false;
      for (auto dynIS : this->indirectStreams) {
        // Merged store stream should not be considered has core user.
        // TODO: The compiler currently failed to set noCoreUser correctly for
        // MergedStore stream, so we ignore it here manually.
        auto IS = dynIS->getStaticStream();
        if (IS->isMerged() && IS->isStoreStream()) {
          continue;
        }
        if (IS->hasCoreUser()) {
          hasCoreUser = true;
          break;
        }
      }
      if (!hasCoreUser) {
        // No core user. Turn off the IssueClearCycle.
        this->shouldUpdateIssueClearCycleMemorized = false;
      }
    }
    // // ! Hack: enforce issue clear cycle to 10 if we have sendTo.
    // if (!this->shouldUpdateIssueClearCycleMemorized &&
    //     !this->sendToConfigs.empty()) {
    //   this->shouldUpdateIssueClearCycleMemorized = true;
    // }
  }

  this->shouldUpdateIssueClearCycleInitialized = true;
  return this->shouldUpdateIssueClearCycleMemorized;
}

void LLCDynamicStream::traceEvent(
    const ::LLVM::TDG::StreamFloatEvent::StreamFloatEventType &type) {
  auto &floatTracer = this->getStaticStream()->floatTracer;
  auto curCycle = this->curCycle();
  assert(this->llcController && "Missing LLCController when tracing event.");
  auto llcBank = this->llcController->getMachineID().num;
  floatTracer.traceEvent(curCycle, llcBank, type);
  // Do this for all indirect streams.
  for (auto IS : this->indirectStreams) {
    IS->traceEvent(type);
  }
}

void LLCDynamicStream::sanityCheckStreamLife() {
  if (!Debug::LLCRubyStreamLife) {
    return;
  }
  bool failed = false;
  if (GlobalLLCDynamicStreamMap.size() > 4096) {
    failed = true;
  }
  if (!failed) {
    return;
  }
  std::vector<LLCDynamicStreamPtr> sortedStreams;
  for (auto &S : GlobalLLCDynamicStreamMap) {
    sortedStreams.push_back(S.second);
  }
  std::sort(sortedStreams.begin(), sortedStreams.end(),
            [](LLCDynamicStreamPtr sa, LLCDynamicStreamPtr sb) -> bool {
              return sa->getDynamicStreamId() < sb->getDynamicStreamId();
            });
  for (auto S : sortedStreams) {
    LLC_S_DPRINTF_(LLCRubyStreamLife, S->getDynamicStreamId(),
                   "Configure %llu LastIssue %llu LastMigrate %llu.\n",
                   S->configureCycle, S->prevIssuedCycle, S->prevMigrateCycle);
  }
  DPRINTF(LLCRubyStreamLife, "Failed StreamLifeCheck at %llu.\n",
          this->curCycle());
  assert(false);
}

bool LLCDynamicStream::allocateElement(uint64_t elementIdx, Addr vaddr) {
  if (this->idxToElementMap.count(elementIdx)) {
    // The element is already allocated.
    return false;
  }
  LLC_S_DPRINTF(this->getDynamicStreamId(), "Allocate element %llu.\n",
                elementIdx);
  if (this->nextElementIdx != elementIdx) {
    LLC_S_PANIC(this->getDynamicStreamId(),
                "Element not allocated in order, next (%llu) != alloc (%llu).",
                this->nextElementIdx, elementIdx);
  }
  auto size = this->getMemElementSize();
  auto element = std::make_shared<LLCStreamElement>(
      this->getStaticStream(), this->llcController, this->getDynamicStreamId(),
      elementIdx, vaddr, size);
  this->idxToElementMap.emplace(elementIdx, element);
  this->nextElementIdx++;

  /**
   * We populate the baseElements. If we are one iteration behind, we are
   * depending on the previous baseElements.
   */
  auto baseElementIdx = elementIdx;
  if (this->isOneIterationBehind()) {
    assert(elementIdx > 0 && "OneIterationBehind StreamElement begins at 1.");
    baseElementIdx = elementIdx - 1;
  }
  for (const auto &baseConfig : this->baseOnConfigs) {
    LLC_S_DPRINTF(this->getDynamicStreamId(), "Add BaseElement from %s.\n",
                  baseConfig->dynamicId);
    const auto &baseDynStreamId = baseConfig->dynamicId;
    auto baseS = baseConfig->stream;
    if (this->baseStream &&
        this->baseStream->getDynamicStreamId() == baseDynStreamId) {
      // This is from base stream.
      assert(this->baseStream->idxToElementMap.count(baseElementIdx) &&
             "Missing BaseElement");
      element->baseElements.emplace_back(
          this->baseStream->idxToElementMap.at(baseElementIdx));
    } else {
      // This is from another bank, we just create the element here.
      // So far we just support remote affine streams.
      auto linearBaseAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
          baseConfig->addrGenCallback);
      assert(linearBaseAddrGen && "Only support remote affine streams.");
      auto baseElementVaddr =
          baseConfig->addrGenCallback
              ->genAddr(baseElementIdx, baseConfig->addrGenFormalParams,
                        getStreamValueFail)
              .front();

      element->baseElements.emplace_back(std::make_shared<LLCStreamElement>(
          baseS, this->llcController, baseDynStreamId, baseElementIdx,
          baseElementVaddr, baseS->getMemElementSize()));
    }
  }
  // Remember to add previous element as base element for reduction.
  if (this->getStaticStream()->isReduction()) {
    if (!this->lastReductionElement) {
      // First time, just initialize the first element.
      this->lastReductionElement = std::make_shared<LLCStreamElement>(
          this->getStaticStream(), this->llcController,
          this->getDynamicStreamId(), 0, 0, size);
      this->lastReductionElement->setValue(
          this->configData->reductionInitValue);
    }
    if (this->lastReductionElement->idx != baseElementIdx) {
      LLC_S_PANIC(
          this->getDynamicStreamId(),
          "Missing previous Reduction LLCStreamElement %llu, Current %llu.",
          baseElementIdx, this->lastReductionElement->idx);
    }
    element->baseElements.emplace_back(this->lastReductionElement);
    this->lastReductionElement = element;
  }

  // We allocate all indirect streams' element here with vaddr 0.
  for (auto &usedByS : this->indirectStreams) {
    auto usedByElementIdx =
        usedByS->isOneIterationBehind() ? (elementIdx + 1) : elementIdx;
    usedByS->allocateElement(usedByElementIdx, 0);
  }
  return true;
}

void LLCDynamicStream::eraseElement(uint64_t elementIdx) {
  auto iter = this->idxToElementMap.find(elementIdx);
  if (iter == this->idxToElementMap.end()) {
    LLC_S_PANIC(this->getDynamicStreamId(), "Failed to erase element %llu.",
                elementIdx);
  }
  LLC_ELEMENT_DPRINTF(iter->second, "Erased.\n");
  this->idxToElementMap.erase(iter);
}

void LLCDynamicStream::eraseElement(IdxToElementMapT::iterator elementIter) {
  LLC_ELEMENT_DPRINTF(elementIter->second, "Erased.\n");
  this->idxToElementMap.erase(elementIter);
}

bool LLCDynamicStream::isBasedOn(const DynamicStreamId &baseId) const {
  for (const auto &baseConfig : this->baseOnConfigs) {
    if (baseConfig->dynamicId == baseId) {
      // Found it.
      return true;
    }
  }
  return false;
}

void LLCDynamicStream::recvStreamForward(LLCStreamEngine *se,
                                         const uint64_t baseElementIdx,
                                         const DynamicStreamSliceId &sliceId,
                                         const DataBlock &dataBlk) {

  auto recvElementIdx = baseElementIdx;
  if (this->isOneIterationBehind()) {
    recvElementIdx++;
  }
  auto S = this->getStaticStream();
  if (!S->isReduction() && !S->isStoreStream()) {
    LLC_S_PANIC(this->getDynamicStreamId(),
                "Recv StreamForward only works for Reduction and StoreStream.");
  }
  if (!this->idxToElementMap.count(recvElementIdx)) {
    LLC_SLICE_DPRINTF(
        sliceId,
        "Cannot find the receiver element %s %llu allocate from %llu.\n",
        this->getDynamicStreamId(), recvElementIdx, this->nextElementIdx);
    assert(this->nextElementIdx <= recvElementIdx &&
           "StreamForward falls behind.");
    assert(this->nextElementIdx + 34 > recvElementIdx &&
           "StreamForward too far ahead.");
    while (this->nextElementIdx <= recvElementIdx) {
      // Should always allocate from base stream.
      auto allocS = this->baseStream ? this->baseStream : this;
      auto allocElementVAddr =
          allocS->slicedStream.getElementVAddr(allocS->nextElementIdx);
      assert(
          allocS->allocateElement(allocS->nextElementIdx, allocElementVAddr) &&
          "Allocation not happen.");
    }
    if (!this->idxToElementMap.count(recvElementIdx)) {
      LLC_S_PANIC(this->getDynamicStreamId(),
                  "Cannot find the receiver element idx %llu, slice %s.",
                  recvElementIdx, sliceId);
    }
  }
  LLCStreamElementPtr recvElement = this->idxToElementMap.at(recvElementIdx);
  bool foundBaseElement = false;
  for (auto &baseElement : recvElement->baseElements) {
    if (baseElement->dynStreamId == sliceId.streamId) {
      // Found the one.
      baseElement->extractElementDataFromSlice(S->getCPUDelegator(), sliceId,
                                               dataBlk);
      foundBaseElement = true;
      break;
    }
  }
  assert(foundBaseElement && "Found base element");
  if (recvElement->areBaseElementsReady()) {
    /**
     * If I am indirect stream, push the computation.
     * Otherwise, the LLC SE should check me for ready elements.
     */
    if (this->baseStream) {
      se->pushReadyComputation(recvElement);
    }
  }
}

std::string LLCDynamicStream::stateToString(State state) {
  switch (state) {
  default:
    panic("Invalid LLCDynamicStream::State %d.", state);
#define Case(x)                                                                \
  case x:                                                                      \
    return #x
    Case(INITIALIZED);
    Case(RUNNING);
    Case(MIGRATING);
    Case(TERMINATED);
#undef Case
  }
}

void LLCDynamicStream::setState(State state) {
  switch (state) {
  default:
    LLC_S_PANIC(this->getDynamicStreamId(),
                "Invalid LLCDynamicStream::State %d.", state);
  case State::RUNNING:
    assert(this->state == State::INITIALIZED ||
           this->state == State::MIGRATING);
    break;
  case State::MIGRATING:
    assert(this->state == State::RUNNING);
    break;
  case State::TERMINATED:
    assert(this->state == State::RUNNING);
    break;
  }
  this->state = state;
}

void LLCDynamicStream::migratingStart() {
  this->setState(State::MIGRATING);
  this->prevMigrateCycle = this->curCycle();
  this->traceEvent(::LLVM::TDG::StreamFloatEvent::MIGRATE_OUT);
  this->getStaticStream()->se->numLLCMigrated++;
}

void LLCDynamicStream::migratingDone(
    AbstractStreamAwareController *llcController) {
  this->llcController = llcController;
  this->setState(State::RUNNING);

  auto &stats = this->getStaticStream()->statistic;
  stats.numLLCMigrate++;
  stats.numLLCMigrateCycle += this->curCycle() - this->prevMigrateCycle;
  this->traceEvent(::LLVM::TDG::StreamFloatEvent::MIGRATE_IN);
}

void LLCDynamicStream::terminate() {
  LLC_S_DPRINTF_(LLCRubyStreamLife, this->getDynamicStreamId(), "Ended.\n");
  this->setState(State::TERMINATED);
  this->traceEvent(::LLVM::TDG::StreamFloatEvent::END);
}

void LLCDynamicStream::allocateLLCStreams(
    AbstractStreamAwareController *mlcController,
    CacheStreamConfigureVec &configs) {
  for (auto &config : configs) {
    LLCDynamicStream::allocateLLCStream(mlcController, config);
  }

  // Remember the allocated group.
  auto mlcNum = mlcController->getMachineID().getNum();
  auto &mlcGroups =
      GlobalMLCToLLCDynamicStreamGroupMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(mlcNum),
                   std::forward_as_tuple())
          .first->second;

  // Try to release old terminated groups.
  assert(mlcGroups.size() < 100 &&
         "Too many MLCGroups, streams are not released?");

  for (auto iter = mlcGroups.begin(), end = mlcGroups.end(); iter != end;) {
    auto &group = *iter;
    bool allTerminated = true;
    for (auto llcS : group) {
      if (!llcS->isTerminated()) {
        allTerminated = false;
        break;
      }
    }
    if (!allTerminated) {
      ++iter;
      continue;
    }
    for (auto &llcS : group) {
      delete llcS;
      llcS = nullptr;
    }
    iter = mlcGroups.erase(iter);
  }

  mlcGroups.emplace_back();
  auto &newGroup = mlcGroups.back();
  for (auto &config : configs) {
    auto llcS = LLCDynamicStream::getLLCStreamPanic(config->dynamicId);
    newGroup.push_back(llcS);
  }
}

void LLCDynamicStream::allocateLLCStream(
    AbstractStreamAwareController *mlcController,
    CacheStreamConfigureDataPtr &config) {

  assert(config->initPAddrValid && "Initial paddr should be valid now.");
  auto initPAddr = config->initPAddr;
  auto mlcMachineId = mlcController->getMachineID();
  auto llcMachineId = mlcController->mapAddressToLLC(
      initPAddr, static_cast<MachineType>(mlcMachineId.type + 1));
  auto llcController =
      AbstractStreamAwareController::getController(llcMachineId);

  // Create the stream.
  auto S = new LLCDynamicStream(mlcController, llcController, config);

  // Check if we have indirect streams.
  for (const auto &edge : config->depEdges) {
    if (edge.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {
      auto &ISConfig = edge.data;
      // Let's create an indirect stream.
      ISConfig->initAllocatedIdx = config->initAllocatedIdx;
      auto IS = new LLCDynamicStream(mlcController, llcController, ISConfig);
      S->indirectStreams.push_back(IS);
      IS->baseStream = S;
    }
  }

  // Create predicated stream information.
  assert(!config->isPredicated && "Base stream should never be predicated.");
  for (auto IS : S->indirectStreams) {
    if (IS->isPredicated()) {
      const auto &predSId = IS->getPredicateStreamId();
      auto predS = LLCDynamicStream::getLLCStream(predSId);
      assert(predS && "Failed to find predicate stream.");
      assert(predS != IS && "Self predication.");
      predS->predicatedStreams.insert(IS);
      IS->predicateStream = predS;
    }
  }
}

Cycles LLCDynamicStream::curCycle() const {
  return this->mlcController->curCycle();
}

int LLCDynamicStream::curLLCBank() const {
  if (this->llcController) {
    return this->llcController->getMachineID().num;
  } else {
    return -1;
  }
}

StreamValue
LLCDynamicStream::computeStreamElementValue(const LLCStreamElementPtr &element,
                                            Cycles &latency) {

  auto S = this->getStaticStream();
  const auto &config = this->configData;

  auto getBaseStreamValue = [&element](uint64_t baseStreamId) -> StreamValue {
    return element->getBaseStreamValue(baseStreamId);
  };

  if (S->isReduction()) {
    // This is a reduction stream.
    assert(element->idx > 0 &&
           "Reduction stream ElementIdx should start at 1.");

    // Perform the reduction.
    latency = config->addrGenCallback->getEstimatedLatency();
    auto newReductionValue = config->addrGenCallback->genAddr(
        element->idx, config->addrGenFormalParams, getBaseStreamValue);
    if (Debug::LLCRubyStreamReduce) {
      std::stringstream ss;
      for (const auto &baseElement : element->baseElements) {
        ss << "\n  " << baseElement->dynStreamId << '-' << baseElement->idx
           << ": " << baseElement->getValue();
      }
      ss << "\n  -> " << element->dynStreamId << '-' << element->idx << ": "
         << newReductionValue;
      LLC_ELEMENT_DPRINTF_(LLCRubyStreamReduce, element,
                           "[Latency %llu] Do reduction %s.\n", latency,
                           ss.str());
    }

    return newReductionValue;

  } else if (S->isStoreStream() && S->getEnabledStoreFunc()) {
    latency = config->storeCallback->getEstimatedLatency();
    auto params = convertFormalParamToParam(config->storeFormalParams,
                                            getBaseStreamValue);
    auto storeValue = config->storeCallback->invoke(params);

    LLC_ELEMENT_DPRINTF_(LLCRubyStreamStore, element,
                         "[Latency %llu] Compute StoreValue %s.\n", latency,
                         storeValue);
    return storeValue;

  } else {
    LLC_ELEMENT_PANIC(element, "No Computation for this stream.");
  }
}

void LLCDynamicStream::completeComputation(LLCStreamEngine *se,
                                           const LLCStreamElementPtr &element,
                                           const StreamValue &value) {
  element->setValue(value);
  this->incompleteComputations--;
  assert(this->incompleteComputations >= 0 &&
         "Negative incomplete computations.");
  /**
   * If this is ReductionStream, check for next element.
   */
  auto S = this->getStaticStream();
  if (S->isReduction()) {
    /**
     * Check for the next element.
     */
    if (this->idxToElementMap.count(element->idx + 1)) {
      auto &nextElement = this->idxToElementMap.at(element->idx + 1);
      if (nextElement->areBaseElementsReady()) {
        se->pushReadyComputation(nextElement);
      }
    }

    /**
     * If this is the last reduction element, we send this back to the core.
     * TODO: Really send a packet to the requesting core.
     */
    if (element->idx == this->configData->totalTripCount) {
      // This is the last reduction.
      auto dynCoreS = S->getDynamicStream(this->getDynamicStreamId());
      assert(dynCoreS && "Core has no dynamic stream.");
      assert(!dynCoreS->finalReductionValueReady &&
             "FinalReductionValue is already ready.");
      dynCoreS->finalReductionValue = value;
      dynCoreS->finalReductionValueReady = true;
      LLC_ELEMENT_DPRINTF_(LLCRubyStreamReduce, element,
                           "Notifiy final reduction.\n");
    }

    /**
     * As a hack here, we release any older elements.
     */
    while (!this->idxToElementMap.empty()) {
      auto iter = this->idxToElementMap.begin();
      if (iter->first > element->idx) {
        break;
      }
      assert(iter->second->isReady() &&
             "Older Reduction Element should be ready.");
      this->eraseElement(iter);
    }
  }
}