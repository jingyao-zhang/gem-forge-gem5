#include "LLCDynamicStream.hh"

#include "cpu/gem_forge/accelerator/stream/stream.hh"
#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "debug/LLCRubyStreamBase.hh"
#include "debug/LLCRubyStreamLife.hh"
#define DEBUG_TYPE LLCRubyStreamBase
#include "../stream_log.hh"

std::unordered_map<DynamicStreamId, LLCDynamicStream *, DynamicStreamIdHasher>
    LLCDynamicStream::GlobalLLCDynamicStreamMap;
std::unordered_map<NodeID, std::list<std::vector<LLCDynamicStream *>>>
    LLCDynamicStream::GlobalMLCToLLCDynamicStreamGroupMap;

// TODO: Support real flow control.
LLCDynamicStream::LLCDynamicStream(
    AbstractStreamAwareController *_mlcController,
    CacheStreamConfigureDataPtr _configData)
    : mlcController(_mlcController), configData(_configData),
      slicedStream(_configData, true /* coalesceContinuousElements */),
      configureCycle(_mlcController->curCycle()), sliceIdx(0),
      allocatedSliceIdx(_configData->initAllocatedIdx), inflyRequests(0) {

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

    auto size = this->getMemElementSize();
    this->lastReductionElement = std::make_shared<LLCStreamElement>(
        this->getStaticStream(), this->getDynamicStreamId(), 0, 0, size);
    this->lastReductionElement->getValue() =
        this->configData->reductionInitValue;
    this->lastReductionElement->readyBytes += size;
    assert(this->lastReductionElement->isReady());
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
  auto curCycle = this->curCycle();
  bool failed = false;
  if (GlobalLLCDynamicStreamMap.size() > 1024) {
    failed = true;
  }
  std::vector<LLCDynamicStreamPtr> sortedStreams;
  for (auto &S : GlobalLLCDynamicStreamMap) {
    sortedStreams.push_back(S.second);
  }
  std::sort(sortedStreams.begin(), sortedStreams.end(),
            [](LLCDynamicStreamPtr sa, LLCDynamicStreamPtr sb) -> bool {
              return sa->getDynamicStreamId() < sb->getDynamicStreamId();
            });
  const Cycles threshold = Cycles(10000);
  for (int i = 0; i < sortedStreams.size(); ++i) {
    auto S = sortedStreams[i];
    auto configCycle = S->configureCycle;
    auto prevIssuedCycle = S->prevIssuedCycle;
    auto prevMigrateCycle = S->prevMigrateCycle;
    if (curCycle - configCycle > threshold &&
        curCycle - prevIssuedCycle > threshold &&
        curCycle - prevMigrateCycle > threshold) {
      if (i + 1 < sortedStreams.size()) {
        // Check if we have new instance of the same static stream.
        auto nextS = sortedStreams.at(i + 1);
        if (nextS->getDynamicStreamId().isSameStaticStream(
                S->getDynamicStreamId())) {
          failed = true;
          break;
        }
      }
    }
  }
  if (!failed) {
    return;
  }
  for (auto S : sortedStreams) {
    LLC_S_DPRINTF_(LLCRubyStreamLife, S->getDynamicStreamId(),
                   "Configure %llu LastIssue %llu LastMigrate %llu.\n",
                   S->configureCycle, S->prevIssuedCycle, S->prevMigrateCycle);
  }
  DPRINTF(LLCRubyStreamLife, "Failed StreamLifeCheck at %llu.\n",
          this->curCycle());
  assert(false);
}

void LLCDynamicStream::allocateElement(uint64_t elementIdx, Addr vaddr) {
  if (this->idxToElementMap.count(elementIdx)) {
    // The element is already allocated.
    return;
  }
  LLC_S_DPRINTF(this->getDynamicStreamId(), "Allocate element %llu.\n",
                elementIdx);
  auto size = this->getMemElementSize();
  auto element = std::make_shared<LLCStreamElement>(this->getStaticStream(),
                                                    this->getDynamicStreamId(),
                                                    elementIdx, vaddr, size);
  this->idxToElementMap.emplace(elementIdx, element);

  LLC_S_DPRINTF(this->getDynamicStreamId(), "Allocate element %llu: added.\n",
                elementIdx);

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
    assert(this->baseStream && "Missing base stream.");
    if (this->baseStream->getDynamicStreamId() == baseDynStreamId) {
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
          baseS, baseDynStreamId, baseElementIdx, baseElementVaddr,
          baseS->getMemElementSize()));
    }
  }
  // Remember to add previous element as base element for reduction.
  if (this->getStaticStream()->isReduction()) {
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

  // Create the stream.
  auto S = new LLCDynamicStream(mlcController, config);

  // Check if we have indirect streams.
  for (const auto &edge : config->depEdges) {
    if (edge.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {
      auto &ISConfig = edge.data;
      // Let's create an indirect stream.
      ISConfig->initAllocatedIdx = config->initAllocatedIdx;
      auto IS = new LLCDynamicStream(mlcController, ISConfig);
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