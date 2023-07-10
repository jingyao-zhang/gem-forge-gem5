#include "LLCDynStream.hh"
#include "LLCStreamCommitController.hh"
#include "LLCStreamEngine.hh"
#include "LLCStreamMigrationController.hh"
#include "LLCStreamRangeBuilder.hh"
#include "MLCStreamEngine.hh"

#include "cpu/gem_forge/accelerator/stream/stream.hh"
#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "debug/LLCRubyStreamBase.hh"
#include "debug/LLCRubyStreamLife.hh"
#include "debug/LLCRubyStreamReduce.hh"
#include "debug/LLCRubyStreamStore.hh"
#include "debug/LLCStreamLoopBound.hh"
#include "debug/LLCStreamPredicate.hh"
#define DEBUG_TYPE LLCRubyStreamBase
#include "../stream_log.hh"

namespace gem5 {

LLCDynStream::GlobalLLCDynStreamMapT LLCDynStream::GlobalLLCDynStreamMap;
std::unordered_map<ruby::NodeID, std::list<std::vector<LLCDynStream *>>>
    LLCDynStream::GlobalMLCToLLCDynStreamGroupMap;

// TODO: Support real flow control.
LLCDynStream::LLCDynStream(ruby::AbstractStreamAwareController *_mlcController,
                           ruby::AbstractStreamAwareController *_llcController,
                           CacheStreamConfigureDataPtr _configData)
    : mlcController(_mlcController), llcController(_llcController),
      maxInflyRequests(_llcController->getLLCStreamMaxInflyRequest()),
      configData(_configData), slicedStream(_configData),
      strandId(_configData->dynamicId, _configData->strandIdx,
               _configData->totalStrands),
      initializedCycle(_mlcController->curCycle()), creditedSliceIdx(0) {

  // Allocate the range builder.
  this->rangeBuilder = std::make_unique<LLCStreamRangeBuilder>(
      this, this->configData->totalTripCount);

  // Remember the SendTo configs.
  for (auto &depEdge : this->configData->depEdges) {
    if (depEdge.type == CacheStreamConfigureData::DepEdge::Type::SendTo) {
      this->sendToEdges.emplace_back(depEdge);
    }
    if (depEdge.type == CacheStreamConfigureData::DepEdge::Type::PUMSendTo) {
      this->sendToPUMEdges.emplace_back(depEdge);
    }
  }

  // Remember the BaseOn configs.
  for (auto &baseEdge : this->configData->baseEdges) {
    // Acquire the shared ptr of the configuration.
    this->baseOnConfigs.emplace_back(baseEdge.data.lock());
    this->reusedBaseStreams.emplace_back(baseEdge.reuse);
    if (baseEdge.reuse <= 0) {
      LLC_S_PANIC(this->strandId, "Illegal Reuse Count %d on %s.",
                  baseEdge.reuse, baseEdge.data.lock()->dynamicId);
    }
    assert(this->baseOnConfigs.back() && "BaseStreamConfig already released?");
    LLC_S_DPRINTF(this->getDynStrandId(), "Add BaseOnConfig %s.\n",
                  this->baseOnConfigs.back()->dynamicId);
  }

  if (this->isPointerChase()) {
    // PointerChase allows at most one InflyRequest.
    this->maxInflyRequests = 1;
  }

  if (_configData->floatPlan.getFirstFloatElementIdx() > 0) {
    auto firstFloatElemIdx = _configData->floatPlan.getFirstFloatElementIdx();
    this->nextCommitElemIdx = firstFloatElemIdx;
    this->nextInitStrandElemIdx = firstFloatElemIdx;
    this->nextAllocElemIdx = firstFloatElemIdx;
    this->nextIssueElemIdx = firstFloatElemIdx;
  }

  // Extract predicated stream information.
  for (const auto &edge : _configData->baseEdges) {
    LLC_S_DPRINTF_(LLCStreamPredicate, this->getDynStrandId(),
                   "[LLCPred] Pred? %d %d = %d %s.\n", edge.isPredBy,
                   edge.predId, edge.predValue, edge.dynStreamId);
    if (edge.isPredBy) {
      assert(!this->isPredBy && "Multi PredBy.");
      this->isPredBy = true;
      this->predId = edge.predId;
      this->predValue = edge.predValue;
      this->predBaseStreamId = edge.dynStreamId;
    }
  }

  if (this->getStaticS()->isReduction() ||
      this->getStaticS()->isPointerChaseIndVar()) {
    // Initialize the first element for ReductionStream with the initial value.
    assert(this->isOneIterationBehind() &&
           "ReductionStream must be OneIterationBehind.");
    this->nextInitStrandElemIdx++;
  }

  /**
   * Release the previous PUMPrefetchStream.
   */
  if (GlobalLLCDynStreamMap.count(this->getDynStrandId())) {
    std::vector<LLCDynStreamPtr> pumPrefetchStreams;
    auto iter = GlobalLLCDynStreamMap.begin();
    while (iter != GlobalLLCDynStreamMap.end()) {
      if (iter->first.dynStreamId == this->getDynStreamId()) {
        auto dynS = iter->second;
        assert(dynS->configData->isPUMPrefetch &&
               "This should be PUMPrefetchStream.");
        assert(dynS->state == State::TERMINATED &&
               "PUMPrefetchStream should be terminated.");
        pumPrefetchStreams.push_back(dynS);
      }
      ++iter;
    }
    for (auto pumPrefetchDynS : pumPrefetchStreams) {
      delete pumPrefetchDynS;
    }
  }

  this->issueBeforeCommit = this->checkIssueBeforeCommit();
  this->issueAfterCommit = this->checkIssueAfterCommit();

  /**
   * Cache the TripCount info here.
   */
  this->totalTripCount = this->slicedStream.getTotalTripCount();
  this->innerTripCount = this->slicedStream.getInnerTripCount();
  LLC_S_DPRINTF(
      this->getDynStrandId(),
      "Created with TotalTripCount %ld InnerTripCount %ld RangeSync %d.\n",
      this->totalTripCount, this->innerTripCount, this->shouldRangeSync());

  [[maybe_unused]] auto emplaced =
      GlobalLLCDynStreamMap.emplace(this->getDynStrandId(), this).second;
  assert(emplaced && "Duplicated LLC Strand.");
  this->sanityCheckStreamLife();
}

LLCDynStream::~LLCDynStream() {
  LLC_S_DPRINTF_(LLCRubyStreamLife, this->getDynStrandId(), "Released.\n");
  auto iter = GlobalLLCDynStreamMap.find(this->getDynStrandId());
  if (iter == GlobalLLCDynStreamMap.end()) {
    LLC_S_PANIC(this->getDynStrandId(),
                "Missed in GlobalLLCDynStreamMap when releaseing.");
  }
  if (!this->baseStream && this->state != LLCDynStream::State::TERMINATED) {
    LLC_S_PANIC(this->getDynStrandId(), "Released DirectStream in %s state.",
                stateToString(this->state));
  }
  if (this->commitController) {
    LLC_S_PANIC(this->getDynStrandId(),
                "Released with registered CommitController.");
  }
  for (auto &indirectStream : this->indirectStreams) {
    delete indirectStream;
    indirectStream = nullptr;
  }
  this->indirectStreams.clear();
  GlobalLLCDynStreamMap.erase(iter);
}

bool LLCDynStream::isInnerLastElem(uint64_t elemIdx) const {
  if (!this->hasInnerTripCount()) {
    return false;
  }
  if (elemIdx == 0) {
    return 0;
  }
  return (elemIdx % this->getInnerTripCount()) == 0;
}

bool LLCDynStream::isLastElem(uint64_t elemIdx) const {
  if (!this->hasTotalTripCount()) {
    return false;
  }
  return elemIdx == this->getTotalTripCount();
}

bool LLCDynStream::shouldSendValueToCore() const {
  auto dynCoreS = this->getCoreDynS();
  return dynCoreS && dynCoreS->shouldCoreSEIssue();
}

void LLCDynStream::setTotalTripCount(int64_t totalTripCount) {
  assert(!this->baseStream && "SetTotalTripCount for IndirectS.");
  this->slicedStream.setTotalAndInnerTripCount(totalTripCount);
  this->rangeBuilder->receiveLoopBoundRet(totalTripCount);
  for (auto dynIS : this->indirectStreams) {
    dynIS->rangeBuilder->receiveLoopBoundRet(totalTripCount *
                                             dynIS->baseStreamReuse);
  }
  // Set for myself and all indirect streams.
  this->totalTripCount = this->slicedStream.getTotalTripCount();
  this->innerTripCount = this->slicedStream.getInnerTripCount();
  for (auto dynIS : this->allIndirectStreams) {
    dynIS->totalTripCount = this->totalTripCount;
    dynIS->innerTripCount = this->innerTripCount;
  }
}

void LLCDynStream::breakOutLoop(int64_t totalTripCount) {
  LLC_S_DPRINTF_(LLCStreamLoopBound, this->getDynStrandId(),
                 "[LoopBound] Break out at %ld.\n", totalTripCount);
  this->loopBoundBrokenOut = true;
  this->setTotalTripCount(totalTripCount);

  // Handle final value for reduction.
  for (auto dynIS : this->allIndirectStreams) {
    if (dynIS->getStaticS()->isReduction()) {
      if (dynIS->lastReducedElemIdx >= totalTripCount) {
        auto se = this->getLLCController()->getLLCStreamEngine();
        dynIS->completeFinalReduce(se, totalTripCount);
      }
    }
  }
}

ruby::MachineType
LLCDynStream::getFloatMachineTypeAtElem(uint64_t elementIdx) const {
  if (this->isOneIterationBehind()) {
    if (elementIdx == 0) {
      LLC_S_PANIC(this->getDynStrandId(),
                  "Get FloatMachineType for Element 0 with OneIterBehind.");
    }
    elementIdx--;
  }
  return this->configData->floatPlan.getMachineTypeAtElem(elementIdx);
}

std::pair<Addr, ruby::MachineType>
LLCDynStream::peekNextInitVAddrAndMachineType() const {
  const auto &sliceId = this->slicedStream.peekNextSlice();
  auto startElemIdx = sliceId.getStartIdx();
  auto startElemMachineType = this->getFloatMachineTypeAtElem(startElemIdx);
  return std::make_pair(sliceId.vaddr, startElemMachineType);
}

const DynStreamSliceId &LLCDynStream::peekNextInitSliceId() const {
  return this->slicedStream.peekNextSlice();
}

uint64_t LLCDynStream::peekNextInitElemIdx() const {
  return this->peekNextInitSliceId().getStartIdx();
}

Addr LLCDynStream::getElementVAddr(uint64_t elementIdx) const {
  return this->slicedStream.getElementVAddr(elementIdx);
}

bool LLCDynStream::translateToPAddr(Addr vaddr, Addr &paddr) const {
  // ! Do something reasonable here to translate the vaddr.
  auto cpuDelegator = this->configData->stream->getCPUDelegator();
  return cpuDelegator->translateVAddrOracle(vaddr, paddr);
}

void LLCDynStream::addCredit(uint64_t n) {
  this->creditedSliceIdx += n;
  for (auto indirectStream : this->getIndStreams()) {
    indirectStream->addCredit(n);
  }
  /**
   * If I am the direct stream, I should notify the RangeBuilder about the next
   * range tail element.
   */
  if (!this->isIndirect()) {
    auto sliceIdx = this->nextAllocSliceIdx;
    auto sliceIter = this->slices.begin();
    auto tailElemIdx = 0;
    while (sliceIdx + 1 <= this->creditedSliceIdx &&
           sliceIter != this->slices.end()) {
      if (sliceIdx + 1 == this->creditedSliceIdx) {
        tailElemIdx = (*sliceIter)->getSliceId().getEndIdx();
        break;
      }
      ++sliceIter;
      ++sliceIdx;
    }
    if (sliceIter == this->slices.end()) {
      LLC_S_PANIC(this->getDynStrandId(),
                  "Missing Slice for RangeBuilder. Credited %lu Alloc %lu "
                  "NewCredit %lu.",
                  this->creditedSliceIdx, this->nextAllocSliceIdx, n);
    }
    LLC_S_DPRINTF(this->getDynStrandId(),
                  "[RangeSync] Add RangeTailElem %lu.\n", tailElemIdx);
    this->addNextRangeTailElemIdx(tailElemIdx);
  }
}

void LLCDynStream::addNextRangeTailElemIdx(uint64_t rangeTailElementIdx) {
  if (this->shouldRangeSync()) {
    this->rangeBuilder->pushNextRangeTailElemIdx(rangeTailElementIdx);
    for (auto dynIS : this->getAllIndStreams()) {
      dynIS->rangeBuilder->pushNextRangeTailElemIdx(rangeTailElementIdx);
    }
  }
}

void LLCDynStream::updateIssueClearCycle() {
  if (!this->shouldUpdateIssueClearCycle()) {
    return;
  }
  const auto *dynS =
      this->configData->stream->getDynStream(this->configData->dynamicId);
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
                                       this->slicedStream.getElemPerSlice();
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
      LLC_S_DPRINTF(this->getDynStrandId(),
                    "Update IssueClearCycle %lu -> %lu (%lu), avgEleTurn %lu, "
                    "avgSliceTurn %lu, avgLateEle %d, elementPerSlice %f.\n",
                    this->issueClearCycle, newIssueClearCycle,
                    IssueClearThreshold, avgTurnAroundCycle,
                    avgSliceTurnAroundCycle, avgLateElements,
                    this->slicedStream.getElemPerSlice());
      this->issueClearCycle =
          Cycles(newIssueClearCycle > IssueClearThreshold ? IssueClearThreshold
                                                          : newIssueClearCycle);
    }
  }
}

bool LLCDynStream::shouldUpdateIssueClearCycle() {
  if (!this->shouldUpdateIssueClearCycleInitialized) {
    // We do not constrain ourselves from the core if there are no core users
    // for both myself and all the indirect streams.
    this->shouldUpdateIssueClearCycleMemorized = true;
    auto dynCoreS = this->getCoreDynS();
    if (dynCoreS && !dynCoreS->shouldCoreSEIssue()) {
      this->shouldUpdateIssueClearCycleMemorized = false;
    }
  }

  this->shouldUpdateIssueClearCycleInitialized = true;
  return this->shouldUpdateIssueClearCycleMemorized;
}

void LLCDynStream::traceEvent(
    const ::LLVM::TDG::StreamFloatEvent::StreamFloatEventType &type) {
  auto &floatTracer = this->getStaticS()->floatTracer;
  auto curCycle = this->curCycle();
  assert(this->llcController && "Missing LLCController when tracing event.");
  auto machineId = this->llcController->getMachineID();
  floatTracer.traceEvent(curCycle, machineId, type);
  // Do this for all indirect streams.
  for (auto IS : this->getAllIndStreams()) {
    IS->traceEvent(type);
  }
}

void LLCDynStream::sanityCheckStreamLife() {
  auto hardLLCStreamThreshold = 100000;
  if (!Debug::LLCRubyStreamLife &&
      GlobalLLCDynStreamMap.size() < hardLLCStreamThreshold) {
    return;
  }
  return;
  bool failed = false;
  if (GlobalLLCDynStreamMap.size() >= hardLLCStreamThreshold) {
    failed = true;
  }
  if (!failed) {
    return;
  }
  std::vector<LLCDynStreamPtr> sortedStreams;
  for (auto &S : GlobalLLCDynStreamMap) {
    sortedStreams.push_back(S.second);
  }
  std::sort(sortedStreams.begin(), sortedStreams.end(),
            [](LLCDynStreamPtr sa, LLCDynStreamPtr sb) -> bool {
              return sa->getDynStreamId() < sb->getDynStreamId();
            });
  for (auto S : sortedStreams) {
    LLC_S_DPRINTF_(
        LLCRubyStreamLife, S->getDynStrandId(),
        "Init %llu LastConfig %llu LastIssue %llu LastMigrate %llu.\n",
        S->initializedCycle, S->prevConfiguredCycle, S->prevIssuedCycle,
        S->prevMigratedCycle);
  }
  DPRINTF(LLCRubyStreamLife, "Failed StreamLifeCheck at %llu.\n",
          this->curCycle());
  assert(false);
}

DynStreamSliceId LLCDynStream::initNextSlice() {
  this->nextInitSliceIdx++;
  return this->slicedStream.getNextSlice();
}

bool LLCDynStream::isNextSliceOverflown() const {
  if (!this->hasTotalTripCount()) {
    return false;
  }
  if (auto slice = this->getNextAllocSlice()) {
    return slice->getSliceId().getStartIdx() >= this->getTotalTripCount();
  }
  return false;
}

bool LLCDynStream::isNextElemOverflown() const {
  if (!this->hasTotalTripCount()) {
    return false;
  }
  auto nextElemIdx = this->peekNextAllocElemIdx();
  return nextElemIdx >= this->getTotalTripCount();
}

LLCStreamSlicePtr LLCDynStream::getNextAllocSlice() const {
  if (this->nextAllocSliceIdx == this->nextInitSliceIdx) {
    // The next slice is not initialized yet.
    return nullptr;
  }
  for (auto &slice : this->slices) {
    if (slice->getState() == LLCStreamSlice::State::INITIALIZED) {
      return slice;
    }
  }
  LLC_S_PANIC(this->getDynStrandId(), "Failed to get NextAllocSlice.");
}

LLCStreamSlicePtr LLCDynStream::allocNextSlice(LLCStreamEngine *se) {
  assert(this->isNextSliceCredited() && "Next slice is not allocated yet.");
  if (auto slice = this->getNextAllocSlice()) {
    slice->allocate(se);
    const auto &sliceId = slice->getSliceId();
    // Add the addr to the RangeBuilder if we have vaddr here.
    if (this->shouldRangeSync()) {
      for (auto elemIdx = sliceId.getStartIdx(); elemIdx < sliceId.getEndIdx();
           ++elemIdx) {
        auto elem = this->getElemPanic(elemIdx, "AllocNextSlice.");
        if (elem->vaddr != 0) {
          Addr paddr = 0;
          if (!this->translateToPAddr(elem->vaddr, paddr)) {
            LLC_S_PANIC(this->getDynStrandId(),
                        "Translation fault on element %llu.", elemIdx);
          }
          if (!elem->hasRangeBuilt()) {
            this->rangeBuilder->addElementAddress(elemIdx, elem->vaddr, paddr,
                                                  elem->size);
            elem->setRangeBuilt();
          }
        }
      }
    }
    LLC_SLICE_DPRINTF(sliceId, "Allocated SliceIdx %llu VAddr %#x.\n",
                      this->nextAllocSliceIdx, slice->getSliceId().vaddr);
    this->invokeSliceAllocCallbacks(this->nextAllocSliceIdx);
    this->nextAllocSliceIdx++;
    this->lastAllocSliceId = sliceId;
    // Slices allocated are now handled by LLCStreamEngine.
    this->slices.pop_front();

    this->checkNextAllocElemIdx();

    return slice;
  }

  LLC_S_PANIC(this->getDynStrandId(), "No Initialized Slice to allocate from.");
}

const DynStreamSliceId &LLCDynStream::peekNextAllocSliceId() const {
  if (auto slice = this->getNextAllocSlice()) {
    return slice->getSliceId();
  } else {
    return this->peekNextInitSliceId();
  }
}

std::pair<Addr, ruby::MachineType>
LLCDynStream::peekNextAllocVAddrAndMachineType() const {
  if (auto slice = this->getNextAllocSlice()) {
    const auto &sliceId = slice->getSliceId();
    auto startElemIdx = sliceId.getStartIdx();
    auto startElemMachineType = this->getFloatMachineTypeAtElem(startElemIdx);
    return std::make_pair(sliceId.vaddr, startElemMachineType);
  } else {
    return this->peekNextInitVAddrAndMachineType();
  }
}

void LLCDynStream::checkNextAllocElemIdx() {
  if (auto slice = this->getNextAllocSlice()) {
    this->nextAllocElemIdx = slice->getSliceId().getStartIdx();
  } else {
    this->nextAllocElemIdx = this->peekNextInitElemIdx();
  }
}

void LLCDynStream::initDirectStreamSlicesUntil(uint64_t lastSliceIdx) {
  if (this->isIndirect()) {
    LLC_S_PANIC(this->getDynStrandId(),
                "InitDirectStreamSlice for IndirectStream.");
  }
  if (this->nextInitSliceIdx >= lastSliceIdx) {
    return;
  }
  while (this->nextInitSliceIdx < lastSliceIdx) {
    auto sliceId = this->initNextSlice();
    auto slice = std::make_shared<LLCStreamSlice>(this->getStaticS(), sliceId);

    // Register the elements.
    while (this->nextInitStrandElemIdx < sliceId.getEndIdx()) {
      this->initNextElem(
          this->slicedStream.getElementVAddr(this->nextInitStrandElemIdx));
    }

    // Remember the mapping from elements to slices.
    for (auto elemIdx = sliceId.getStartIdx(); elemIdx < sliceId.getEndIdx();
         ++elemIdx) {
      auto elem = this->getElemPanic(elemIdx, "InitDirectSlice");
      elem->addSlice(slice);
    }

    // Push into our queue.
    LLC_SLICE_DPRINTF(sliceId, "DirectStreamSlice Initialized.\n");
    this->slices.push_back(slice);
  }
}

void LLCDynStream::initNextElem(Addr vaddr) {
  const auto strandElemIdx = this->nextInitStrandElemIdx;
  const auto streamElemIdx =
      this->configData->getStreamElemIdxFromStrandElemIdx(strandElemIdx);
  LLC_S_DPRINTF(this->getDynStrandId(),
                "Initialize StrandElem %llu StreamElem %llu.\n", strandElemIdx,
                streamElemIdx);

  /**
   * Some sanity check for large AffineStream that we don't have too many
   * elements.
   * However, I have encountered a rare case: the data is presented in MLC,
   * so MLCStream get advanced as request hit in private cache, but the LLC
   * lags behind as the latency to get the copy at the LLC is too long,
   * delaying element releasing.
   * Therefore, I increased the threshold and also check for elements with
   * slices not released.
   */
  if (this->getStaticS()->isDirectMemStream() &&
      this->getMemElementSize() >= 64) {
    if (this->idxToElementMap.size() >= 2048) {
      int sliceNotReleasedElements = 0;
      for (const auto &entry : this->idxToElementMap) {
        const auto &element = entry.second;
        if (!element->areSlicesReleased()) {
          sliceNotReleasedElements++;
        }
      }
      if (sliceNotReleasedElements > 128) {
        for (const auto &entry : this->idxToElementMap) {
          const auto &elem = entry.second;
          LLC_ELEMENT_HACK(elem,
                           "Elem Overflow. Ready %d. AllSlicedReleased %d.\n",
                           elem->isReady(), elem->areSlicesReleased());
          for (int i = 0, nSlices = elem->getNumSlices(); i < nSlices; ++i) {
            const auto &slice = elem->getSliceAt(i);
            Addr sliceVAddr = slice->getSliceId().vaddr;
            Addr slicePAddr = 0;
            this->translateToPAddr(sliceVAddr, slicePAddr);
            LLC_ELEMENT_HACK(elem, "  Slice %s %s VAddr %#x PAddr %#x.",
                             slice->getSliceId(),
                             LLCStreamSlice::stateToString(slice->getState()),
                             sliceVAddr, slicePAddr);
          }
        }
        auto MLCSE = this->getMLCController()->getMLCStreamEngine();
        auto MLCDynS = MLCSE->getStreamFromStrandId(this->getDynStrandId());
        if (MLCDynS) {
          MLCDynS->panicDump();
        } else {
          LLC_S_HACK(this->getDynStrandId(),
                     "LLCElem Overflow, but MLCDynS Released?");
        }
        LLC_S_PANIC(this->getDynStrandId(), "Infly Elem Overflow %d.",
                    this->idxToElementMap.size());
      }
    }
  }

  auto size = this->getMemElementSize();
  // AtomicComputeS will have CoreElemSize.
  if (this->getStaticS()->isAtomicComputeStream()) {
    size = this->getCoreElementSize();
  }
  auto elem = std::make_shared<LLCStreamElement>(
      this->getStaticS(), this->mlcController, this->getDynStrandId(),
      strandElemIdx, vaddr, size, false /* isNDCElement */);
  this->idxToElementMap.emplace(strandElemIdx, elem);
  this->nextInitStrandElemIdx++;

  auto translateDepToBase =
      [this, streamElemIdx, strandElemIdx,
       &elem](const CacheStreamConfigureData::BaseEdge &baseEdge,
              const CacheStreamConfigureDataPtr &baseConfig)
      -> std::tuple<DynStrandId, uint64_t, uint64_t> {
    /**
     * Translate to base strand.
     * NOTE: Here we need to adjust BaseStreamElemIdx with ReuseCount.
     * NOTE: Be careful with StreamElemIdx and StrandElemIdx.
     * NOTE: After splitted into strands, IsOneIterBehind actually means one
     * StrandElem behind. This changed the dependence chain, and should only be
     * applied to Reduction (Not PtrChase).
     */
    DynStrandId baseStrandId;
    uint64_t baseStrandElemIdx;
    uint64_t baseStreamElemIdx;
    if (baseEdge.isStrandSendTo) {
      // Strand SendTo. No need to convert to stream.
      assert(!this->isOneIterationBehind());
      baseStrandId = baseConfig->getStrandId();
      baseStrandElemIdx = CacheStreamConfigureData::convertDepToBaseElemIdx(
          strandElemIdx, baseEdge.reuse, baseEdge.reuseTileSize, baseEdge.skip);
      baseStreamElemIdx = baseConfig->getStreamElemIdxFromStrandElemIdx(
          baseStrandId, baseStrandElemIdx);

      LLC_ELEMENT_DPRINTF(
          elem, "Add R/S %ld/%ld -> BaseStrnd %s%lu -> BaseStrm %lu.\n",
          baseEdge.reuse, baseEdge.skip, baseStrandId, baseStrandElemIdx,
          baseStreamElemIdx);

    } else {
      auto depStreamElemIdx = streamElemIdx;
      if (this->isOneIterationBehind()) {
        assert(strandElemIdx > 0 &&
               "OneIterationBehind StreamElement begins at 1.");
        depStreamElemIdx = this->configData->getStreamElemIdxFromStrandElemIdx(
            strandElemIdx - 1);
      }

      baseStreamElemIdx = CacheStreamConfigureData::convertDepToBaseElemIdx(
          depStreamElemIdx, baseEdge.reuse, baseEdge.reuseTileSize,
          baseEdge.skip);

      baseStrandId =
          baseConfig->getStrandIdFromStreamElemIdx(baseStreamElemIdx);
      baseStrandElemIdx =
          baseConfig->getStrandElemIdxFromStreamElemIdx(baseStreamElemIdx);

      LLC_ELEMENT_DPRINTF(
          elem,
          "Add DepStrm %lu R/S %ld/%ld -> BaseStrm %lu BaseStrnd %s%lu.\n",
          depStreamElemIdx, baseEdge.reuse, baseEdge.skip, baseStreamElemIdx,
          baseStrandId, baseStrandElemIdx);
    }

    return std::make_tuple(baseStrandId, baseStrandElemIdx, baseStreamElemIdx);
  };

  for (auto i = 0; i < this->baseOnConfigs.size(); ++i) {

    /**
     * We populate the baseElements. If we are one iteration behind, we are
     * depending on the previous baseElements.
     * Adjust the BaseElemIdx to ReuseCount.
     * NOTE: Here we need to adjust BaseStreamElemIdx with ReuseCount.
     * NOTE: Be careful with StreamElemIdx and StrandElemIdx.
     * NOTE: After splitted into strands, IsOneIterBehind actually means one
     * StrandElem behind. This changed the dependence chain, and should only be
     * applied to Reduction (Not PtrChase).
     */

    const auto &baseEdge = this->configData->baseEdges.at(i);
    const auto &baseConfig = this->baseOnConfigs.at(i);
    auto &reusedBaseS = this->reusedBaseStreams.at(i);

    auto translation = translateDepToBase(baseEdge, baseConfig);
    const auto &baseStrandId = std::get<0>(translation);
    const auto &baseStrandElemIdx = std::get<1>(translation);
    const auto &baseStreamElemIdx = std::get<2>(translation);

    const auto &baseDynStreamId = baseConfig->dynamicId;
    auto baseS = baseConfig->stream;
    // Search through the FloatChain.
    LLCDynStreamPtr onFloatChainBaseDynS = nullptr;
    {
      auto curBaseDynS = this->baseStream;
      while (curBaseDynS) {
        if (curBaseDynS->getDynStreamId() == baseDynStreamId) {
          onFloatChainBaseDynS = curBaseDynS;
          break;
        }
        curBaseDynS = curBaseDynS->baseStream;
      }
    }
    if (onFloatChainBaseDynS) {
      /**
       * This is from a base stream on the FloatChain. Check that the mapping is
       * homogeneous if splitted.
       */
      if (this->configData->isSplitIntoStrands()) {
        if (baseStrandId.strandIdx != this->getDynStrandId().strandIdx) {
          LLC_ELEMENT_PANIC(elem, "Mismatch IndStrandIdx with BaseStrandId %s.",
                            baseStrandId);
        }
        auto baseStrandElemIdxOffset = this->isOneIterationBehind() ? 1 : 0;
        if (baseStrandElemIdx + baseStrandElemIdxOffset !=
            strandElemIdx / baseEdge.reuse) {
          LLC_ELEMENT_PANIC(
              elem,
              "Mismatch IndStrandElemIdx with BaseStrandId %s%lu Offset %lu.",
              baseStrandId, baseStrandElemIdx, baseStrandElemIdxOffset);
        }
      }
      if (!onFloatChainBaseDynS->idxToElementMap.count(baseStrandElemIdx)) {
        LLC_ELEMENT_PANIC(elem, "Missing base elem from %s.",
                          onFloatChainBaseDynS->getDynStrandId());
      }
      elem->baseElements.emplace_back(
          onFloatChainBaseDynS->idxToElementMap.at(baseStrandElemIdx));
    } else {
      /**
       * This is not from our AddrBaseS, and we need to create the element here.
       * Two cases:
       *
       * 1. A MemS from another bank. If this is a remote affine stream, we
       * directly get the ElementVAddr. Otherwise, we set the vaddr to 0 and
       * delay setting it in recvStreamForwawrd().
       *
       * 2. A UsedAffineIV. We directly set the value here.
       *
       * If the ReuseCount > 1 we check if we can borrow from allocated element.
       * If the ReuseCount is 1, we allocate the element.
       * Otherwise, and we have the same
       */
      LLCStreamElementPtr baseElem = nullptr;
      if (reusedBaseS.reuse > 1 && reusedBaseS.hasElem(baseStreamElemIdx)) {
        // We can reuse.
        baseElem = reusedBaseS.reuseElem(baseStreamElemIdx);
      } else {

        Addr baseElemVAddr = 0;
        auto linearBaseAddrGen =
            std::dynamic_pointer_cast<LinearAddrGenCallback>(
                baseConfig->addrGenCallback);

        if (!baseEdge.isUsedAffineIV && linearBaseAddrGen) {
          auto addrGenBaseElemIdx = baseStreamElemIdx;
          if (baseEdge.isStrandSendTo) {
            addrGenBaseElemIdx = baseStrandElemIdx;
          }
          baseElemVAddr =
              baseConfig->addrGenCallback
                  ->genAddr(addrGenBaseElemIdx, baseConfig->addrGenFormalParams,
                            getStreamValueFail)
                  .front();
          LLC_ELEMENT_DPRINTF(elem, "BaseStrandElem %s%llu VAddr %#x.\n",
                              baseStrandId, baseStrandElemIdx, baseElemVAddr);
        }

        baseElem = std::make_shared<LLCStreamElement>(
            baseS, this->mlcController, baseStrandId, baseStrandElemIdx,
            baseElemVAddr, baseS->getMemElementSize(),
            false /* isNDCElement */);

        if (baseEdge.isUsedAffineIV) {
          assert(linearBaseAddrGen &&
                 "UsedAffineIV should have LinearAddrGen.");

          // Generate the value.
          auto value = baseConfig->addrGenCallback->genAddr(
              baseStreamElemIdx, baseConfig->addrGenFormalParams,
              getStreamValueFail);

          // Set the value.
          baseElem->setValue(value);

          assert(baseElem->isReady() && "UsedAffineIV should be ready.");
        }
      }
      elem->baseElements.emplace_back(baseElem);
      // Update the ReusedStreamElem.
      reusedBaseS.addElem(baseStreamElemIdx, baseElem);
    }
  }
  // Remember to add previous element as base element for reduction.
  if (this->getStaticS()->isReduction() ||
      this->getStaticS()->isPointerChaseIndVar()) {
    if (!this->lastReductionElement) {
      uint64_t firstStreamElemIdx =
          this->configData->floatPlan.getFirstFloatElementIdx();

      if (firstStreamElemIdx > 0 && this->configData->isSplitIntoStrands()) {
        LLC_S_PANIC(this->getDynStreamId(),
                    "Strand with FirstStreamElemIdx %lu > 0 is not working.",
                    firstStreamElemIdx);
      }

      // First time, just initialize the first element.
      this->lastReductionElement = std::make_shared<LLCStreamElement>(
          this->getStaticS(), this->mlcController, this->getDynStrandId(),
          firstStreamElemIdx, 0, size, false /* isNDCElement */);
      this->lastReductionElement->setValue(
          this->configData->reductionInitValue);
      this->lastReducedElemIdx = firstStreamElemIdx;

      // If we have LoopBound. the first elem is considered done.
      if (this->hasLoopBound()) {
        this->lastReductionElement->doneLoopBound();
      }

      // Also add the first element to the map.
      this->idxToElementMap.emplace(firstStreamElemIdx,
                                    this->lastReductionElement);
    }

    assert(this->isOneIterationBehind());
    assert(strandElemIdx > 0);
    auto baseStrandElemIdx = strandElemIdx - 1;

    if (this->lastReductionElement->idx != baseStrandElemIdx) {
      LLC_S_PANIC(
          this->getDynStrandId(),
          "Missing previous Reduction StrandElemIdx %llu, Current %llu.",
          baseStrandElemIdx, this->lastReductionElement->idx);
    }
    /**
     * IndirectReductionStream is handled differently, as their computation
     * latency is charged at each indirect banks, but to keep things simple,
     * the real computation is still carried out serially.
     *
     * Therefore, we do not add PrevReduceElem as the base element for
     * IndirectReduceS. Also, since the compute value is not truly
     * ready, we have to avoid any user of the reduction value.
     */
    if (this->isIndirectReduction()) {
      if (!this->configData->depEdges.empty()) {
        LLC_S_PANIC(this->getDynStrandId(),
                    "Dependence of IndirectReduceS is not supported.");
      }
    } else {
      // Direct reduction.
      elem->baseElements.emplace_back(this->lastReductionElement);
    }
    elem->setPrevReduceElem(this->lastReductionElement);
    this->lastReductionElement = elem;
  }

  /**
   * We call ElementInitCallback here.
   */
  auto elemInitCbIter = this->elemInitCallbacks.find(strandElemIdx);
  if (elemInitCbIter != this->elemInitCallbacks.end()) {
    for (auto &callback : elemInitCbIter->second) {
      callback(this->getDynStrandId(), strandElemIdx);
    }
    this->elemInitCallbacks.erase(elemInitCbIter);
  }

  /**
   * We allocate all indirect streams' element here with vaddr 0.
   * Take care with possible reuse.
   */
  for (auto indS : this->getIndStreams()) {
    auto reuse = indS->baseStreamReuse;
    for (auto j = 0; j < reuse; ++j) {
      indS->initNextElem(0);
    }
  }
}

bool LLCDynStream::isElemInitialized(uint64_t elemIdx) const {
  return elemIdx < this->nextInitStrandElemIdx;
}

void LLCDynStream::registerElemInitCallback(uint64_t elementIdx,
                                            ElementCallback callback) {
  if (this->isElemInitialized(elementIdx)) {
    LLC_S_PANIC(this->getDynStrandId(),
                "Register ElementInitCallback for InitializedElement %llu.",
                elementIdx);
  }
  this->elemInitCallbacks
      .emplace(std::piecewise_construct, std::forward_as_tuple(elementIdx),
               std::forward_as_tuple())
      .first->second.push_back(callback);
}

void LLCDynStream::registerElemPostReleaseCallback(uint64_t elemIdx,
                                                   ElementCallback callback) {
  if (this->isElemReleased(elemIdx)) {
    LLC_S_PANIC(this->getDynStrandId(),
                "Register ElemPostReleaseCallback for Elem %llu.", elemIdx);
  }
  this->elemPostReleaseCallbacks
      .emplace(std::piecewise_construct, std::forward_as_tuple(elemIdx),
               std::forward_as_tuple())
      .first->second.push_back(callback);
}

void LLCDynStream::registerSliceAllocCallback(uint64_t sliceIdx,
                                              SliceCallback callback) {
  if (this->getNextAllocSliceIdx() >= sliceIdx) {
    LLC_S_PANIC(this->getDynStrandId(),
                "Register SliceAllocCallback for AllocatedSlice %llu.",
                sliceIdx);
  }
  this->sliceAllocCallbacks
      .emplace(std::piecewise_construct, std::forward_as_tuple(sliceIdx),
               std::forward_as_tuple())
      .first->second.push_back(callback);
}

void LLCDynStream::invokeSliceAllocCallbacks(uint64_t sliceIdx) {
  /**
   * We call ElementInitCallback here.
   */
  auto iter = this->sliceAllocCallbacks.find(sliceIdx);
  if (iter != this->sliceAllocCallbacks.end()) {
    for (auto &callback : iter->second) {
      LLC_S_DPRINTF(this->getDynStrandId(),
                    "[SliceAllocCallback] Invoke %llu.\n", sliceIdx);
      callback(this->getDynStreamId(), sliceIdx);
    }
    this->sliceAllocCallbacks.erase(iter);
  }
}

bool LLCDynStream::isElemReleased(uint64_t elementIdx) const {
  if (this->idxToElementMap.empty()) {
    return elementIdx < this->nextInitStrandElemIdx;
  }
  return this->idxToElementMap.begin()->first > elementIdx;
}

uint64_t LLCDynStream::getNextUnreleasedElemIdx() const {
  return this->idxToElementMap.empty() ? this->getNextInitElementIdx()
                                       : this->idxToElementMap.begin()->first;
}

void LLCDynStream::eraseElem(uint64_t elemIdx) {
  auto iter = this->idxToElementMap.find(elemIdx);
  if (iter == this->idxToElementMap.end()) {
    LLC_S_PANIC(this->getDynStrandId(), "Failed to erase elem %lu.", elemIdx);
  }
  this->eraseElem(iter);
}

void LLCDynStream::eraseElem(IdxToElementMapT::iterator elemIter) {
  auto elem = elemIter->second;
  LLC_ELEMENT_DPRINTF(elem, "Erased elem. Remaining %lu. TotalAlive %lu.\n",
                      this->idxToElementMap.size() - 1,
                      LLCStreamElement::getAliveElems());
  auto S = this->getStaticS();
  S->incrementOffloadedStepped();
  if (this->hasLoopBound() && !elem->isLoopBoundDone()) {
    /**
     * We erased an element without LoopBound evaluated.
     * The only case should be that we are over the TotalTripCount after some
     * previous element breaks out the loop.
     */
    if (!this->hasTotalTripCount()) {
      LLC_SE_ELEM_PANIC(
          elem, "[LoopBound] Erased wo. LoopBoundDone. No TotalTripCount.");
    }
    if (elem->idx < this->getTotalTripCount()) {
      LLC_SE_ELEM_PANIC(
          elem, "[LoopBound] Erased wo. LoopBoundDone < TotalTripCount %lu.",
          this->getTotalTripCount());
    }
  }
  /**
   * Right now each Element has a shared_ptr to the base element. However,
   * this forms a long chain of ReduceElem, and consumes crazy amount of
   * memory. To fix that, we clear the base elements when releasing an
   * element.
   */
  elem->baseElements.clear();
  elem->setPrevReduceElem(nullptr);
  auto elemIdx = elemIter->first;
  this->idxToElementMap.erase(elemIter);
  this->invokeElemPostReleaseCallback(elemIdx);
}

void LLCDynStream::invokeElemPostReleaseCallback(uint64_t elemIdx) {

  /**
   * We call ElementReleaseCallback here.
   */
  auto iter = this->elemPostReleaseCallbacks.find(elemIdx);
  if (iter != this->elemPostReleaseCallbacks.end()) {
    for (auto &callback : iter->second) {
      callback(this->getDynStrandId(), elemIdx);
    }
    this->elemPostReleaseCallbacks.erase(iter);
  }
}

bool LLCDynStream::isBasedOn(const DynStreamId &baseId) const {
  for (const auto &baseConfig : this->baseOnConfigs) {
    if (baseConfig->dynamicId == baseId) {
      // Found it.
      return true;
    }
  }
  return false;
}

void LLCDynStream::recvStreamForward(LLCStreamEngine *se, int offset,
                                     const DynStreamSliceId &sliceId,
                                     const DynStreamSliceId &sendToSliceId,
                                     const ruby::DataBlock &dataBlk) {
  auto sendStrandElemIdx = sliceId.getStartIdx() + offset;
  auto recvStrandElemIdx = sendToSliceId.getStartIdx() + offset;
  bool found = false;
  for (const auto &baseEdge : this->configData->baseEdges) {
    if (baseEdge.dynStreamId == sliceId.getDynStreamId()) {

      // OneIterBehind is defined on Strand.
      if (this->isOneIterationBehind()) {
        recvStrandElemIdx++;
      }

      LLC_S_DPRINTF(this->getDynStrandId(),
                    "[Fwd] Recv Send %s%lu by R/S %d/%d = Recv %lu.\n",
                    sliceId.getDynStrandId(), sendStrandElemIdx, baseEdge.reuse,
                    baseEdge.skip, recvStrandElemIdx);

      found = true;
      break;
    }
  }

  panic_if(!found, "Failed to Find SendConfig");

  auto S = this->getStaticS();
  if (!this->idxToElementMap.count(recvStrandElemIdx)) {
    LLC_SLICE_PANIC(
        sliceId, "Cannot find the RecvStrandElem %s%lu allocate from %lu.\n",
        this->getDynStrandId(), recvStrandElemIdx, this->nextInitStrandElemIdx);
  }
  LLCStreamElementPtr recvElem = this->idxToElementMap.at(recvStrandElemIdx);
  bool foundBaseElem = false;
  for (auto &baseElem : recvElem->baseElements) {
    if (baseElem->strandId == sliceId.getDynStrandId()) {
      /**
       * Found the base element to hold the data.
       * If the BaseS is IndS, we copy the ElemVaddr from the slice.
       */
      if (baseElem->vaddr == 0) {
        baseElem->vaddr = sliceId.vaddr;
      }
      LLC_S_DPRINTF(
          this->getDynStrandId(),
          "[Fwd] Extract BaseElem %s%lu VAddr %#x Size %d SliceVAddr %#x.\n",
          baseElem->strandId, baseElem->idx, baseElem->vaddr, baseElem->size,
          sliceId.vaddr);
      baseElem->extractElementDataFromSlice(S->getCPUDelegator(), sliceId,
                                            dataBlk);
      if (baseElem->S->isLoadComputeStream()) {
        // Also extract the LoadComputeValue.
        baseElem->extractComputedValueFromSlice(S->getCPUDelegator(), sliceId,
                                                dataBlk);
      }
      foundBaseElem = true;
      break;
    }
  }
  if (!foundBaseElem) {
    for (auto &baseElem : recvElem->baseElements) {
      LLC_ELEMENT_DPRINTF(recvElem, "BaseElem %s %lu.\n", baseElem->strandId,
                          baseElem->idx);
    }
    LLC_ELEMENT_PANIC(recvElem, "Failed to find BaseElem %s.",
                      sliceId.getDynStrandId());
  }
  /**
   * Try to self trigger IndElem.
   * Otherwise, the LLC SE should check me for ready elements.
   */
  if (this->isIndirect()) {
    se->triggerIndElem(this, recvElem->idx);
  }
}

std::string LLCDynStream::stateToString(State state) {
  switch (state) {
  default:
    panic("Invalid LLCDynStream::State %d.", state);
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

void LLCDynStream::setState(State state) {
  switch (state) {
  default:
    LLC_S_PANIC(this->getDynStrandId(), "Invalid LLCDynStream::State %d.",
                state);
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

void LLCDynStream::remoteConfigured(
    ruby::AbstractStreamAwareController *llcCtrl) {
  this->setState(State::RUNNING);
  this->setLLCController(llcCtrl);
  assert(this->prevConfiguredCycle == Cycles(0) && "Already RemoteConfigured.");
  this->prevConfiguredCycle = this->curCycle();
  auto &stats = this->getStaticS()->statistic;
  stats.numRemoteConfig++;
  stats.numRemoteConfigNoCCycle +=
      this->prevConfiguredCycle - this->initializedCycle;
  LLC_S_DPRINTF_(LLCRubyStreamLife, this->getDynStrandId(),
                 "RemoteConfig at %s.\n", llcCtrl->getMachineID());
  if (auto *dynS = this->getStaticS()->getDynStream(this->getDynStreamId())) {
    stats.numRemoteConfigCycle += this->prevConfiguredCycle - dynS->configCycle;
  }
}

void LLCDynStream::migratingStart(
    ruby::AbstractStreamAwareController *nextLLCCtrl) {
  this->setState(State::MIGRATING);
  this->nextLLCController = nextLLCCtrl;
  // Recursively set for all IndS.
  for (auto IS : this->getAllIndStreams()) {
    IS->setState(State::MIGRATING);
    IS->nextLLCController = nextLLCCtrl;
  }
  this->prevMigratedCycle = this->curCycle();
  this->traceEvent(::LLVM::TDG::StreamFloatEvent::MIGRATE_OUT);
  this->getStaticS()->se->numLLCMigrated++;

  LLC_S_DPRINTF_(LLCRubyStreamLife, this->getDynStrandId(),
                 "Migrate from %s.\n", this->llcController->getMachineID());
  auto &stats = this->getStaticS()->statistic;
  stats.numRemoteRunCycle +=
      this->prevMigratedCycle - this->prevConfiguredCycle;
}

void LLCDynStream::setLLCController(
    ruby::AbstractStreamAwareController *llcController) {
  this->llcController = llcController;
  // Set the llcController for all indirect streams.
  for (auto dynIS : this->allIndirectStreams) {
    dynIS->llcController = llcController;
  }
}

void LLCDynStream::migratingDone(ruby::AbstractStreamAwareController *llcCtrl) {

  /**
   * Notify the previous LLC SE that I have arrived.
   */
  assert(this->llcController && "Missing PrevLLCController after migration.");
  auto prevSE = this->llcController->getLLCStreamEngine();
  auto &prevMigrateController = prevSE->migrateController;
  prevMigrateController->migratedTo(this, llcCtrl->getMachineID());

  LLC_S_DPRINTF_(LLCRubyStreamLife, this->getDynStrandId(), "Migrated to %s.\n",
                 llcCtrl->getMachineID());

  this->setLLCController(llcCtrl);
  this->setState(State::RUNNING);
  for (auto IS : this->getAllIndStreams()) {
    IS->setState(State::RUNNING);
  }
  this->prevConfiguredCycle = this->curCycle();

  auto &stats = this->getStaticS()->statistic;
  stats.numRemoteMigrate++;
  stats.numRemoteMigrateCycle +=
      this->prevConfiguredCycle - this->prevMigratedCycle;
  this->traceEvent(::LLVM::TDG::StreamFloatEvent::MIGRATE_IN);
}

void LLCDynStream::terminate() {
  LLC_S_DPRINTF_(LLCRubyStreamLife, this->getDynStrandId(), "Ended.\n");
  this->setState(State::TERMINATED);
  for (auto IS : this->getAllIndStreams()) {
    IS->setState(State::TERMINATED);
  }
  this->traceEvent(::LLVM::TDG::StreamFloatEvent::END);
  if (this->commitController) {
    // Don't forget to deregister myself from commit controller.
    this->commitController->deregisterStream(this);
  }
  // Remember the last run cycles.
  auto &stats = this->getStaticS()->statistic;
  stats.numRemoteRunCycle += this->curCycle() - this->prevConfiguredCycle;
}

void LLCDynStream::allocateLLCStreams(
    ruby::AbstractStreamAwareController *mlcController,
    CacheStreamConfigureVec &configs) {

  /**
   * By default we set the LoadBalanceValve on the StoreComputeStream or
   * UpdateStream with smallest StaticId. If no such streams, we set on the
   * first LLCDynStreams.
   */
  LLCDynStreamPtr uncuttedLLCDynSWithSmallestStatidId = nullptr;
  std::vector<LLCDynStreamPtr> loadBalanceValueStreams;
  for (auto &config : configs) {
    auto S = LLCDynStream::allocateLLCStream(mlcController, config);
    if (!config->hasBeenCuttedByMLC) {
      if (!uncuttedLLCDynSWithSmallestStatidId ||
          uncuttedLLCDynSWithSmallestStatidId->getStaticId() >
              S->getStaticId()) {
        uncuttedLLCDynSWithSmallestStatidId = S;
      }
    }
    if (S->getStaticS()->isStoreComputeStream() ||
        S->getStaticS()->isUpdateStream()) {
      loadBalanceValueStreams.push_back(S);
    }
  }
  if (loadBalanceValueStreams.empty()) {
    assert(uncuttedLLCDynSWithSmallestStatidId && "Configured not LLCDynS.");
  } else {
    std::sort(loadBalanceValueStreams.begin(), loadBalanceValueStreams.end(),
              [](const LLCDynStreamPtr &A, const LLCDynStreamPtr &B) -> bool {
                if (A->getStaticId() == B->getStaticId()) {
                  return A->getDynStrandId().strandIdx <
                         B->getDynStrandId().strandIdx;
                }
                return A->getStaticId() < B->getStaticId();
              });
    auto loadBalanceDynS = loadBalanceValueStreams.front();
    /**
     * We need to set all strands as the LoadBalance stream,
     * but not for PUMPrefetch stream.
     */
    if (!loadBalanceDynS->configData->isPUMPrefetch) {
      // loadBalanceDynS->setLoadBalanceValve();
      for (auto dynS : loadBalanceValueStreams) {
        if (dynS->getStaticId() != loadBalanceDynS->getStaticId()) {
          // This is not the same static stream.
          break;
        }
        DPRINTF(LLCRubyStreamBase, "%s: [LoadBalance] Selected.\n",
                dynS->getDynStrandId());
        dynS->setLoadBalanceValve();
      }
    }
  }

  // Remember the allocated group.
  auto mlcNum = mlcController->getMachineID().getNum();
  auto &mlcGroups =
      GlobalMLCToLLCDynStreamGroupMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(mlcNum),
                   std::forward_as_tuple())
          .first->second;

  // Try to release old terminated groups.
  const auto mlcGroupThreshold = 100;
  if (mlcGroups.size() == mlcGroupThreshold) {
    DPRINTF(LLCRubyStreamLife, "[MLCGroup] Overflow.\n");
    int i = 0;
    for (const auto &group : mlcGroups) {
      DPRINTF(LLCRubyStreamLife, "[MLCGroup]   Group %d.\n", i);
      for (auto llcS : group) {
        DPRINTF(LLCRubyStreamLife, "[MLCGroup]      %s Terminated %d.\n",
                llcS->getDynStreamId(), llcS->isTerminated());
      }
      ++i;
    }
    if (mlcGroups.size() >= mlcGroupThreshold) {
      panic("Too many MLCGroups for %s.", configs.front()->dynamicId);
    }
  }

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
    DPRINTF(LLCRubyStreamLife, "Release MLCGroup %d.\n", mlcNum);
    for (auto &llcS : group) {
      DPRINTF(LLCRubyStreamLife, "Release MLCGroup: %s.\n",
              llcS->getDynStrandId());
      if (mlcNum != llcS->getDynStreamId().coreId) {
        panic("LLCStream %s released from wrong MLCGroup %d.",
              llcS->getDynStrandId(), mlcNum);
      }
      delete llcS;
      llcS = nullptr;
    }
    DPRINTF(LLCRubyStreamLife, "Release MLCGroup %d done.\n", mlcNum);
    iter = mlcGroups.erase(iter);
  }

  mlcGroups.emplace_back();
  auto &newGroup = mlcGroups.back();
  for (auto &config : configs) {
    auto llcS = LLCDynStream::getLLCStreamPanic(DynStrandId(
        config->dynamicId, config->strandIdx, config->totalStrands));
    DPRINTF(LLCRubyStreamLife, "Push into MLCGroup %d: %s.\n", mlcNum,
            llcS->getDynStreamId());
    if (mlcNum != llcS->getDynStreamId().coreId) {
      panic("LLCStream %s pushed into wrong MLCGroup %d.",
            llcS->getDynStreamId(), mlcNum);
    }
    newGroup.push_back(llcS);
  }
}

LLCDynStreamPtr LLCDynStream::allocateLLCStream(
    ruby::AbstractStreamAwareController *mlcController,
    CacheStreamConfigureDataPtr &config) {

  assert(config->initPAddrValid && "Initial paddr should be valid now.");
  auto initPAddr = config->initPAddr;

  // Get the ruby::MachineType of FirstFloatElementIdx.
  auto firstFloatElemIdx = config->floatPlan.getFirstFloatElementIdx();
  auto firstFloatElemMachineType =
      config->floatPlan.getMachineTypeAtElem(firstFloatElemIdx);

  auto llcMachineId =
      mlcController->mapAddressToLLCOrMem(initPAddr, firstFloatElemMachineType);
  auto llcController =
      ruby::AbstractStreamAwareController::getController(llcMachineId);

  // Create the stream.
  auto S = new LLCDynStream(mlcController, llcController, config);

  // DFS to allocate indirect streams.
  {
    std::vector<CacheStreamConfigureDataPtr> configStack;
    configStack.push_back(config);
    while (!configStack.empty()) {
      auto curConfig = configStack.back();
      auto curS = LLCDynStream::getLLCStreamPanic(
          DynStrandId(curConfig->dynamicId, curConfig->strandIdx),
          "Miss BaseS.");
      configStack.pop_back();
      for (const auto &edge : curConfig->depEdges) {
        if (edge.type != CacheStreamConfigureData::DepEdge::Type::UsedBy) {
          continue;
        }

        auto &ISConfig = edge.data;
        // Let's create an indirect stream.
        ISConfig->initCreditedIdx = config->initCreditedIdx;
        auto IS = new LLCDynStream(mlcController, llcController, ISConfig);
        // Can not handle reused tile for IndS for now.
        assert(edge.reuseTileSize == 1);
        IS->setBaseStream(curS, edge.reuse);
        configStack.push_back(ISConfig);
      }
    }
  }

  // Initialize the first slices.
  // For some strands, it's possible that InitCreditedIdx is 0 as the TotalTrip
  // is 0.
  if (config->initCreditedIdx > 0) {
    S->initDirectStreamSlicesUntil(config->initCreditedIdx);
  }

  return S;
}

void LLCDynStream::setBaseStream(LLCDynStreamPtr baseS, int reuse) {
  if (this->baseStream) {
    LLC_S_PANIC(this->getDynStrandId(), "Set multiple base LLCDynStream.");
  }
  this->baseStream = baseS;
  this->baseStreamReuse = reuse;
  this->rootStream = baseS->rootStream ? baseS->rootStream : baseS;
  baseS->indirectStreams.push_back(this);
  baseS->allIndirectStreams.push_back(this);
  if (baseS != this->rootStream) {
    this->rootStream->allIndirectStreams.push_back(this);
  }
  // Copy the RootStream's TripCount.
  if (!this->hasTotalTripCount()) {
    this->totalTripCount = this->rootStream->totalTripCount;
    this->innerTripCount = this->rootStream->innerTripCount;
  }
}

Cycles LLCDynStream::curCycle() const {
  return this->mlcController->curCycle();
}

int LLCDynStream::curRemoteBank() const {
  if (this->llcController) {
    return this->llcController->getMachineID().num;
  } else {
    return -1;
  }
}

const char *LLCDynStream::curRemoteMachineType() const {
  if (this->llcController) {
    auto type = this->llcController->getMachineID().type;
    if (type == ruby::MachineType_L2Cache) {
      return "LLC";
    } else if (type == ruby::MachineType_Directory) {
      return "MEM";
    }
  }
  return "XXX";
}

ruby::AbstractStreamAwareController *LLCDynStream::curOrNextRemoteCtrl() const {
  if (this->state == State::MIGRATING) {
    assert(this->nextLLCController);
    return this->nextLLCController;
  } else {
    assert(this->llcController);
    return this->llcController;
  }
}

bool LLCDynStream::hasComputation() const {
  auto S = this->getStaticS();
  return S->isReduction() || S->isPointerChaseIndVar() ||
         S->isLoadComputeStream() || S->isStoreComputeStream() ||
         S->isUpdateStream() || S->isAtomicComputeStream();
}

StreamValue LLCDynStream::computeElemValue(const LLCStreamElementPtr &elem) {

  auto S = elem->S;
  const auto &config = this->configData;

  auto getBaseStreamValue = [&elem](uint64_t baseStreamId) -> StreamValue {
    return elem->getBaseStreamValue(baseStreamId);
  };
  auto getBaseOrMyStreamValue = [&elem](uint64_t baseStreamId) -> StreamValue {
    return elem->getBaseOrMyStreamValue(baseStreamId);
  };
  if (S->isReduction() || S->isPointerChaseIndVar()) {
    // This is a reduction stream.
    assert(elem->idx > 0 && "Reduction stream ElementIdx should start at 1.");

    // Perform the reduction.
    auto getBaseOrPrevReductionStreamValue =
        [&elem, this](uint64_t baseStreamId) -> StreamValue {
      if (elem->S->isCoalescedHere(baseStreamId)) {
        auto prevReduceElem = elem->getPrevReduceElem();
        assert(prevReduceElem && "Missing prev reduction element.");
        assert(prevReduceElem->isReady() && "PrevReduceElem is not ready.");

        /**
         * Special case for computing ReduceS: If the PrevReductionElem is
         * InnerLastElem, we will use the InitialValue.
         * This is the case when the ReduceS is configured at OuterLoop.
         */
        if (elem->S->isReduction() &&
            this->isInnerLastElem(prevReduceElem->idx)) {
          return this->configData->reductionInitValue;
        }

        return prevReduceElem->getValueByStreamId(baseStreamId);
      }
      return elem->getBaseStreamValue(baseStreamId);
    };

    Cycles latency = S->getEstimatedComputationLatency();
    auto newReduceVal =
        config->addrGenCallback->genAddr(elem->idx, config->addrGenFormalParams,
                                         getBaseOrPrevReductionStreamValue);
    if (Debug::LLCRubyStreamReduce) {
      std::stringstream ss;
      for (const auto &baseElement : elem->baseElements) {
        ss << "\n  " << baseElement->strandId << baseElement->idx << ": "
           << baseElement->getValue(0, baseElement->size);
      }
      ss << "\n  -> " << elem->strandId << elem->idx << ": " << newReduceVal;
      LLC_ELEMENT_DPRINTF_(LLCRubyStreamReduce, elem,
                           "[Lat %llu] Do reduction %s.\n", latency,
                           ss.str());
    }

    return newReduceVal;

  } else if (S->isStoreComputeStream()) {
    Cycles latency = config->storeCallback->getEstimatedLatency();
    auto params = convertFormalParamToParam(config->storeFormalParams,
                                            getBaseStreamValue);
    auto storeValue = config->storeCallback->invoke(params);

    LLC_ELEMENT_DPRINTF_(LLCRubyStreamStore, elem,
                         "[Lat %llu] Compute StoreValue %s.\n", latency,
                         storeValue);
    return storeValue;

  } else if (S->isLoadComputeStream()) {
    Cycles latency = config->loadCallback->getEstimatedLatency();
    auto params = convertFormalParamToParam(config->loadFormalParams,
                                            getBaseOrMyStreamValue);
    auto loadComputeValue = config->loadCallback->invoke(params);

    LLC_ELEMENT_DPRINTF_(LLCRubyStreamStore, elem,
                         "[Lat %llu] Compute LoadComputeValue %s.\n",
                         latency, loadComputeValue);
    return loadComputeValue;

  } else if (S->isUpdateStream()) {

    Cycles latency = config->storeCallback->getEstimatedLatency();
    auto params = convertFormalParamToParam(config->storeFormalParams,
                                            getBaseOrMyStreamValue);
    auto storeValue = config->storeCallback->invoke(params);

    LLC_ELEMENT_DPRINTF_(LLCRubyStreamStore, elem,
                         "[Lat %llu] Compute StoreComputeValue %s.\n",
                         latency, storeValue);
    return storeValue;

  } else if (S->isAtomicComputeStream()) {

    // So far Atomic are really computed at completeComputation.
    LLC_ELEMENT_DPRINTF_(LLCRubyStreamStore, elem,
                         "[Lat %llu] Compute Dummy AtomicValue.\n",
                         S->getEstimatedComputationLatency());
    return StreamValue();

  } else {
    LLC_ELEMENT_PANIC(elem, "No Computation for this stream.");
  }
}

void LLCDynStream::completeComputation(LLCStreamEngine *se,
                                       const LLCStreamElementPtr &elem,
                                       const StreamValue &value) {
  auto S = this->getStaticS();
  elem->doneComputation();
  /**
   * LoadCompute/Update store computed value in ComputedValue.
   * AtomicComputeStream has no computed value.
   * IndirectReductionStream separates compuation from charging the latency.
   */
  if (S->isLoadComputeStream() || S->isUpdateStream()) {
    elem->setComputedValue(value);
    this->evaluatePredication(se, elem->idx);
    this->evaluateLoopBound(se, elem->idx);
    se->triggerIndElems(this, elem);
  } else if (S->isAtomicComputeStream()) {

  } else if (!this->isIndirectReduction()) {
    elem->setValue(value);
  }
  if (!elem->isComputationVectorized()) {
    this->incompleteComputations--;
    assert(this->incompleteComputations >= 0 &&
           "Negative incomplete computations.");
  }

  const auto seMachineID = se->controller->getMachineID();
  auto floatMachineType = this->getFloatMachineTypeAtElem(elem->idx);
  if (seMachineID.getType() != floatMachineType) {
    LLC_ELEMENT_PANIC(elem,
                      "[CompleteCmp] Offload %s != SE ruby::MachineType %s.",
                      floatMachineType, seMachineID);
  }

  /**
   * For UpdateStream, we store here.
   */
  if (S->isUpdateStream()) {

    // Perform the operation.
    auto elemMemSize = S->getMemElementSize();
    auto elemVAddr = elem->vaddr;

    Addr elemPAddr;
    panic_if(!this->translateToPAddr(elemVAddr, elemPAddr),
             "Fault on vaddr of UpdateStream.");
    const auto lineSize = ruby::RubySystem::getBlockSizeBytes();

    assert(elemMemSize <= sizeof(value) && "UpdateStream size overflow.");
    for (int storedSize = 0; storedSize < elemMemSize;) {
      Addr vaddr = elemVAddr + storedSize;
      Addr paddr;
      if (!this->translateToPAddr(vaddr, paddr)) {
        LLC_ELEMENT_PANIC(elem, "Fault on vaddr of UpdateStream.");
      }
      auto lineOffset = vaddr % lineSize;
      auto size = elemMemSize - storedSize;
      if (lineOffset + size > lineSize) {
        size = lineSize - lineOffset;
      }
      se->performStore(paddr, size, value.uint8Ptr(storedSize));
      storedSize += size;
    }
    LLC_ELEMENT_DPRINTF_(LLCRubyStreamStore, elem,
                         "StreamUpdate done with value %s.\n", value);

  } else if (S->isAtomicComputeStream()) {

    // Ask the SE for post processing.
    assert(!S->isDirectMemStream() &&
           "Only IndirectAtomic will complete computation.");
    se->postProcessIndirectAtomicSlice(this, elem);

  } else if (S->isReduction() || S->isPointerChaseIndVar()) {
    if (this->isIndirectReduction()) {
      /**
       * If this is IndirectReduceS, perform the real computation.
       */
      this->completeIndReduceElem(se, elem);
      this->tryComputeNextIndirectReduceElem(se);
    } else {
      /**
       * If this is DirectReduceS, check and schedule the next element.
       */
      if (this->lastReducedElemIdx + 1 != elem->idx) {
        LLC_S_PANIC(this->getDynStrandId(),
                    "[DirectReduce] Reduction not in order.\n");
      }
      this->lastReducedElemIdx++;
      // Evaluate the loop bound.
      this->evaluateLoopBound(se, this->lastReducedElemIdx);

      // Trigger indirect elements.
      se->triggerIndElems(this, elem);

      // If the last reduction element is ready, we send this back to the core.
      if (this->isInnerLastElem(this->lastReducedElemIdx) ||
          this->isLastElem(this->lastReducedElemIdx)) {
        this->completeFinalReduce(se, this->lastReducedElemIdx);
      }
    }

    /**
     * As a hack here, we release any older elements that are ready.
     */
    while (!this->idxToElementMap.empty()) {
      auto iter = this->idxToElementMap.begin();
      if (!iter->second->isReady()) {
        break;
      }
      this->eraseElem(iter);
    }

    /**
     * This has to be at the end of completeComputation,
     * as LLC SE may skip the computation and call
     * LLCDynStream::completeComputation() immediately.
     */
    if (!this->isIndirectReduction()) {
      this->tryComputeNextDirectReduceElem(se, elem);
    }
  }
}

void LLCDynStream::tryComputeNextDirectReduceElem(
    LLCStreamEngine *se, const LLCStreamElementPtr &elem) {

  const auto seMachineID = se->controller->getMachineID();
  if (this->idxToElementMap.count(elem->idx + 1)) {
    auto &nextElem = this->idxToElementMap.at(elem->idx + 1);
    if (nextElem->areBaseElemsReady()) {
      /**
       * We need to push the computation to the LLC SE at the correct
       * bank.
       */
      LLCStreamEngine *nextComputeSE = se;
      for (const auto &baseElement : nextElem->baseElements) {
        if (baseElement->strandId != this->baseStream->getDynStrandId()) {
          continue;
        }
        auto vaddr = baseElement->vaddr;
        if (vaddr != 0) {
          Addr paddr;
          panic_if(!this->baseStream->translateToPAddr(vaddr, paddr),
                   "Failed to translate for NextReductionBaseElement.");
          auto llcMachineID = this->mlcController->mapAddressToLLCOrMem(
              paddr, seMachineID.getType());
          nextComputeSE =
              ruby::AbstractStreamAwareController::getController(llcMachineID)
                  ->getLLCStreamEngine();
        }
      }

      /**
       * Due to vectorization, this compuation may be skipped
       * and immediately complete. To avoid recursive hell, make sure
       * tryComputeNextDirectReduceElem is at the tail of completeComputation().
       */
      nextComputeSE->pushReadyComputation(nextElem);
    } else {
      for (const auto &baseE : nextElem->baseElements) {
        LLC_ELEMENT_DPRINTF(nextElem, "BaseElems Ready %d %s%llu.\n",
                            baseE->isReady(), baseE->strandId, baseE->idx);
      }
    }
  } else {
    LLC_ELEMENT_DPRINTF(elem, "NextElem not initialized.\n");
  }
}

void LLCDynStream::tryComputeNextIndirectReduceElem(LLCStreamEngine *se) {
  /**
   * IndirectReduce is separated into fake reduction at each IndBank, and real
   * reduction that is still serialized. Here we try to compute the real one.
   * Notice that here we charge zero latency, as we already charged it
   * when schedule the computation.
   */
  LLC_S_DPRINTF_(
      LLCRubyStreamReduce, this->getDynStrandId(),
      "[IndReduce] Start real computation from LastReducedElem %llu.\n",
      this->lastReducedElemIdx);
  while (true) {
    auto reduceElemIdx = this->lastReducedElemIdx + 1;
    if (this->hasTotalTripCount() &&
        reduceElemIdx > this->getTotalTripCount()) {
      LLC_S_DPRINTF_(
          LLCRubyStreamReduce, this->getDynStrandId(),
          "[IndReduce] NextReduceElem %llu > TripCount %ld. Break.\n",
          reduceElemIdx, this->getTotalTripCount());
      break;
    }
    auto reduceElem = this->getElem(reduceElemIdx);
    if (!reduceElem) {
      LLC_S_DPRINTF_(LLCRubyStreamReduce, this->getDynStrandId(),
                     "[IndReduce] Missing NextReduceElem %llu. Break.\n",
                     reduceElemIdx);
      break;
    }
    if (!reduceElem->isComputationDone()) {
      LLC_S_DPRINTF_(LLCRubyStreamReduce, this->getDynStrandId(),
                     "[IndReduce] NextReduceElem %llu not done. Break.\n",
                     reduceElemIdx);
      break;
    }
    // Really do the computation.
    LLC_S_DPRINTF_(LLCRubyStreamReduce, this->getDynStrandId(),
                   "[IndReduce] Really computed %llu.\n", reduceElemIdx);
    auto result = this->computeElemValue(reduceElem);
    reduceElem->setValue(result);
    this->lastReducedElemIdx++;

    // Try to send back the final reduced value.
    if (this->isLastElem(reduceElemIdx)) {
      this->completeFinalReduce(se, reduceElemIdx);
    }
  }
}

void LLCDynStream::completeFinalReduce(LLCStreamEngine *se, uint64_t elemIdx) {

  // This is the last reduction of one inner loop.
  auto S = this->getStaticS();
  auto finalReduceElem =
      this->getElemPanic(elemIdx, "Return FinalReductionValue.");

  DynStreamSliceId sliceId;
  sliceId.vaddr = 0;
  sliceId.size = finalReduceElem->size;
  sliceId.getDynStrandId() = this->getDynStrandId();
  sliceId.getStartIdx() = elemIdx;
  sliceId.getEndIdx() = elemIdx + 1;
  auto finalReductionValue = finalReduceElem->getValueByStreamId(S->staticId);

  Addr paddrLine = 0;
  int dataSize = sizeof(finalReductionValue);
  int payloadSize = finalReduceElem->size;
  int lineOffset = 0;
  bool forceIdea = false;

  if (this->configData->finalValueNeededByCore) {
    se->issueStreamDataToMLC(sliceId, paddrLine, finalReductionValue.uint8Ptr(),
                             dataSize, payloadSize, lineOffset, forceIdea);
    LLC_ELEMENT_DPRINTF_(LLCRubyStreamReduce, finalReduceElem,
                         "[Reduce] Send result back to core.\n");
  }

  // We also want to forward to any consuming streams.
  ruby::DataBlock dataBlock;
  dataBlock.setData(finalReductionValue.uint8Ptr(), lineOffset, payloadSize);

  LLC_ELEMENT_DPRINTF_(LLCRubyStreamReduce, finalReduceElem,
                       "[Reduce] Forward result.\n");
  se->issueStreamDataToLLC(
      this, sliceId, dataBlock, this->sendToEdges,
      ruby::RubySystem::getBlockSizeBytes() /* PayloadSize */);
}

void LLCDynStream::completeIndReduceElem(LLCStreamEngine *se,
                                         const LLCStreamElementPtr &elem) {
  /**
   * If we enabled distributed indirect reduce, we don't need to
   * do anything here. The final value is supposed to be collected by a final
   * multicast message (not implemented yet).
   *
   * Otherwise, we approximate the effect of sending back the data by sending
   * back an Ack message for each indirect reduce element.
   */
  if (se->controller->myParams->enable_distributed_indirect_reduce) {
    return;
  }
  DynStreamSliceId sliceId;
  sliceId.vaddr = 0;
  sliceId.size = elem->size;
  sliceId.getDynStrandId() = this->getDynStrandId();
  sliceId.getStartIdx() = elem->idx;
  sliceId.getEndIdx() = elem->idx + 1;

  bool forceIdea = false;
  se->issueStreamAckToMLC(sliceId, forceIdea);
  LLC_ELEMENT_DPRINTF_(LLCRubyStreamReduce, elem,
                       "[IndReduce] Send Ack back to core.\n");
}

bool LLCDynStream::isNextIdeaAck() const {
  if (this->shouldRangeSync()) {
    return false;
  }
  if (this->isIndirect()) {
    // IndirectS can ack at reuse granularity.
    if ((this->streamAckedSlices % this->baseStreamReuse) != 0) {
      return true;
    }
  } else {
    // DirectS can ack at segment granularity.
    int slicesPerSegment = std::max(
        1, this->configData->mlcBufferNumSlices /
               this->mlcController->myParams
                   ->mlc_stream_slices_runahead_inverse_ratio /
               this->mlcController->getMLCStreamBufferToSegmentRatio());

    // Be conservative that last slice is never ideally acked.
    auto nextNonIdeaAckSegment =
        (this->streamAckedSlices + slicesPerSegment - 1) / slicesPerSegment;
    if (nextNonIdeaAckSegment * slicesPerSegment >= this->nextInitSliceIdx) {
      return false;
    }

    if ((this->streamAckedSlices % slicesPerSegment) != 0) {
      return true;
    }
  }
  return false;
}

void LLCDynStream::addCommitMessage(const DynStreamSliceId &sliceId) {
  auto iter = this->commitMessages.begin();
  auto end = this->commitMessages.end();
  while (iter != end) {
    if (iter->getStartIdx() > sliceId.getStartIdx()) {
      break;
    }
    ++iter;
  }
  this->commitMessages.insert(iter, sliceId);
  // Now we mark the elements committed.
  for (auto elementIdx = sliceId.getStartIdx();
       elementIdx < sliceId.getEndIdx(); ++elementIdx) {
    auto element =
        this->getElemPanic(elementIdx, "MarkCoreCommit for LLCElement");
    element->setCoreCommitted();
  }
}

void LLCDynStream::commitOneElement() {
  if (this->hasTotalTripCount() &&
      this->nextCommitElemIdx >= this->getTotalTripCount()) {
    LLC_S_PANIC(this->getDynStrandId(),
                "[Commit] Commit element %llu beyond TotalTripCount %llu.\n",
                this->nextCommitElemIdx, this->getTotalTripCount());
  }
  this->nextCommitElemIdx++;
  for (auto dynIS : this->getIndStreams()) {
    dynIS->commitOneElement();
  }
}

void LLCDynStream::markElemReadyToIssue(uint64_t elemIdx) {
  auto elem =
      this->getElemPanic(elemIdx, "Mark IndirectElement ready to issue.");
  if (elem->getState() != LLCStreamElement::State::INITIALIZED) {
    LLC_ELEMENT_PANIC(elem, "IndElem in wrong state  %d to mark ready.",
                      elem->getState());
  }

  if (this->getStaticS()->isReduction() ||
      this->getStaticS()->isPointerChaseIndVar()) {
    LLC_S_PANIC(this->getDynStrandId(),
                "Reduction is not handled as issue for now.");
    return;
  }
  /**
   * We directly compute the address here.
   * So that we can notify our range builder.
   */
  auto getBaseStreamValue = [&elem](uint64_t baseStreamId) -> StreamValue {
    return elem->getBaseStreamValue(baseStreamId);
  };
  Addr elemVAddr = this->configData->addrGenCallback
                       ->genAddr(elemIdx, this->configData->addrGenFormalParams,
                                 getBaseStreamValue)
                       .front();
  LLC_ELEMENT_DPRINTF(elem, "Generate indirect vaddr %#x, size %d.\n",
                      elemVAddr, this->getMemElementSize());
  elem->vaddr = elemVAddr;
  elem->setStateToReadyToIssue(this->mlcController->curCycle());

  this->numElemsReadyToIssue++;
  // Increment the counter in the root stream.
  if (this->rootStream) {
    this->rootStream->numDepIndElemsReadyToIssue++;
  }
}

void LLCDynStream::markElemIssued(uint64_t elemIdx) {
  if (elemIdx != this->nextIssueElemIdx) {
    LLC_S_PANIC(this->getDynStrandId(), "IndElem should be issued in order.");
  }
  auto elem = this->getElemPanic(elemIdx, "Mark IndirectElement issued.");
  if (elem->getState() != LLCStreamElement::State::READY_TO_ISSUE) {
    LLC_S_PANIC(this->getDynStrandId(),
                "IndirectElement %llu not in ready state.", elemIdx);
  }
  /**
   * Notify the RangeBuilder.
   */
  if (this->shouldRangeSync()) {
    Addr paddr;
    if (!this->translateToPAddr(elem->vaddr, paddr)) {
      LLC_S_PANIC(this->getDynStrandId(), "Fault on Issued element %llu.",
                  elemIdx);
    }
    this->rangeBuilder->addElementAddress(elemIdx, elem->vaddr, paddr,
                                          elem->size);
    elem->setRangeBuilt();
  }
  elem->setStateToIssued(this->mlcController->curCycle());
  assert(this->numElemsReadyToIssue > 0 &&
         "Underflow NumElementsReadyToIssue.");
  this->numElemsReadyToIssue--;
  this->nextIssueElemIdx++;
  /**
   * Skip the next one elem if it is predicated off.
   */
  this->skipIssuingPredOffElems();
  if (this->rootStream) {
    assert(this->rootStream->numDepIndElemsReadyToIssue > 0 &&
           "Underflow NumIndirectElementsReadyToIssue.");
    this->rootStream->numDepIndElemsReadyToIssue--;
  }
}

void LLCDynStream::skipIssuingPredOffElems() {
  while (true) {
    auto elem = this->getElem(this->nextIssueElemIdx);
    if (!elem || !elem->isPredicatedOff()) {
      break;
    }
    LLC_ELEMENT_DPRINTF_(LLCStreamPredicate, elem,
                         "[LLCPred] Skip Issuing PredOff Elem.\n");
    this->nextIssueElemIdx++;
  }
}

LLCStreamElementPtr LLCDynStream::getFirstReadyToIssueElem() const {
  if (this->numElemsReadyToIssue == 0) {
    return nullptr;
  }
  auto elem = this->getElemPanic(this->nextIssueElemIdx, __func__);
  switch (elem->getState()) {
  default:
    LLC_S_PANIC(this->getDynStrandId(), "Elem %llu with Invalid state %d.",
                elem->idx, elem->getState());
  case LLCStreamElement::State::INITIALIZED:
    // To guarantee in-order, return false here.
    return nullptr;
  case LLCStreamElement::State::READY_TO_ISSUE:
    return elem;
  case LLCStreamElement::State::ISSUED:
    LLC_S_PANIC(this->getDynStrandId(), "NextIssueElem %llu already issued.",
                elem->idx);
  }
}

bool LLCDynStream::checkIssueBeforeCommit() const {
  if (!this->shouldRangeSync()) {
    return true;
  }
  /**
   * The only case we don't have to issue BeforeCommit is:
   * 1. I have no indirect streams dependent on myself.
   * 2. I also have no SendTo dependence.
   * 3. I am a StoreStream or AtomicStream and the core does
   * not need the value.
   */
  if (this->hasIndirectDependent()) {
    return true;
  }
  if (!this->sendToEdges.empty()) {
    return true;
  }
  auto S = this->getStaticS();
  if (S->isAtomicComputeStream()) {
    auto dynS = this->getCoreDynS();
    if (!dynS) {
      LLC_S_DPRINTF(this->getDynStrandId(), "[IssueBeforeCommit] No dynS.\n");
    } else {
      auto shouldCoreSEIssue = dynS->shouldCoreSEIssue();
      LLC_S_DPRINTF(this->getDynStrandId(),
                    "[IssueBeforeCommit] CoreSE Issue %d.\n",
                    shouldCoreSEIssue);
      if (!shouldCoreSEIssue) {
        return false;
      }
    }
  }
  return true;
}

bool LLCDynStream::checkIssueAfterCommit() const {
  if (!this->shouldRangeSync()) {
    return false;
  }
  // We need to issue after commit if we are writing to memory.
  auto S = this->getStaticS();
  if (S->isAtomicComputeStream()) {
    return true;
  }
  return false;
}

void LLCDynStream::evaluatePredication(LLCStreamEngine *se, uint64_t elemIdx) {

  /**
   * For now, predication is evaluated ideally.
   * TODO: Push into ComputeEngine.
   */
  if (!this->hasPredication()) {
    return;
  }

  auto elem = this->getElem(elemIdx);
  if (!elem) {
    LLC_S_PANIC(this->getDynStrandId(), "[LLCPred] No Elem %llu.", elemIdx);
  }
  if (!elem->isUsedValueReady()) {
    LLC_ELEMENT_DPRINTF_(LLCStreamPredicate, elem, "[LLCPred] Not ready.\n");
    return;
  }
  if (elem->isPredValueReady()) {
    LLC_ELEMENT_DPRINTF_(LLCStreamPredicate, elem,
                         "[LLCPred] Already evaluated.\n");
    return;
  }

  auto getStreamValue = [&elem](uint64_t streamId) -> StreamValue {
    return elem->getUsedBaseOrMyStreamValue(streamId);
  };
  const auto &preds = this->configData->predCallbacks;
  for (int predId = 0; predId < preds.size(); ++predId) {
    const auto &pred = preds.at(predId);
    auto params = convertFormalParamToParam(pred.formalParams, getStreamValue);
    auto predRet = pred.func->invoke(params).front();

    LLC_ELEMENT_DPRINTF_(LLCStreamPredicate, elem, "[LLCPred] Pred %d = %d.\n",
                         predId, predRet);
    elem->setPredValue(predId, predRet);
  }
  elem->markPredValueReady();
}

void LLCDynStream::evaluateLoopBound(LLCStreamEngine *se, uint64_t elemIdx) {
  if (!this->hasLoopBound()) {
    return;
  }

  auto elem = this->getElem(elemIdx);
  if (!elem) {
    if (this->isElemReleased(elemIdx)) {
      LLC_S_PANIC(this->getDynStrandId(), "[LLCLoopBound] Elem %llu released.",
                  elemIdx);
    } else {
      // Not allocated yet.
      return;
    }
  }
  if (elem->isLoopBoundDone()) {
    LLC_SE_ELEM_DPRINTF_(LLCStreamLoopBound, elem,
                         "[LLCLoopBound] Already done.\n");
    return;
  }
  if (!elem->isUsedValueReady()) {
    LLC_SE_ELEM_DPRINTF_(LLCStreamLoopBound, elem,
                         "[LLCLoopBound] not ready.\n");
    return;
  }

  /**
   * If the LoopBound is associated with ReduceS, need to evaluate with
   * nextElem's baseElem.
   */
  auto S = this->getStaticS();
  bool isReduceOrPtrChaseIndVar = S->isReduction() || S->isPointerChaseIndVar();
  auto tripCount = elemIdx;
  if (isReduceOrPtrChaseIndVar) {
    assert(elemIdx > 0);
    tripCount = elemIdx - 1;
  }
  auto getStreamValue =
      [&elem, isReduceOrPtrChaseIndVar](uint64_t streamId) -> StreamValue {
    if (isReduceOrPtrChaseIndVar) {
      return elem->getBaseStreamValue(streamId);
    } else {
      return elem->getUsedBaseOrMyStreamValue(streamId);
    }
  };
  auto loopBoundActualParams = convertFormalParamToParam(
      this->configData->loopBoundFormalParams, getStreamValue);
  auto loopBoundRet =
      this->configData->loopBoundCallback->invoke(loopBoundActualParams)
          .front();

  elem->doneLoopBound();

  if (loopBoundRet == this->configData->loopBoundRet) {
    /**
     * We should break.
     * So far we just magically set TotalTripCount for Core/MLC/LLC.
     * TODO: Handle this in a more realistic way.
     */
    LLC_SE_ELEM_DPRINTF_(LLCStreamLoopBound, elem,
                         "[LLCLoopBound] Break (%d == %d) TripCount %llu.\n",
                         loopBoundRet, this->configData->loopBoundRet,
                         tripCount);

    se->sendLoopBoundRetToMLC(this, tripCount, true /* broken */);
    if (this->rootStream) {
      this->loopBoundBrokenOut = true;
    } else {
      this->loopBoundBrokenOut = true;
    }

  } else {
    /**
     * We should continue.
     * Currently the core would just be blocking to wait wait for the
     * the final results. However, the current implementation still
     * steps through all the elements. To avoid a burst of stepping
     * after we determine the TotalTripCount, here we also notify
     * the core SE that you can step this "fake" element.
     */
    LLC_SE_ELEM_DPRINTF_(LLCStreamLoopBound, elem,
                         "[LLCLoopBound] Continue (%d != %d).\n", loopBoundRet,
                         this->configData->loopBoundRet);
    se->sendLoopBoundRetToMLC(this, tripCount, false /* broken */);
  }
}

bool LLCDynStream::isSliceDoneForLoopBound(
    const DynStreamSliceId &sliceId) const {

  if (!this->hasLoopBound()) {
    return true;
  }

  if (this->isLoopBoundBrokenOut()) {
    return true;
  }

  for (auto elemIdx = sliceId.getStartIdx(); elemIdx < sliceId.getEndIdx();
       ++elemIdx) {
    /**
     * It is possible that we need to evaluate LoopBound for this slice.
     * Due to multi-line elements, we check that this slice completes the
     * element, i.e. it contains the last byte of this element.
     * In such case, we evalute the LoopBound and not release the slice.
     */
    auto elem = this->getElemPanic(elemIdx, "Check element for LoopBound.");
    if (elem->isLoopBoundDone()) {
      continue;
    }
    if (!elem->isLastSlice(sliceId)) {
      return false;
    }
  }
  return true;
}

LLCStreamElementPtr LLCDynStream::getElem(uint64_t elementIdx) const {
  auto iter = this->idxToElementMap.find(elementIdx);
  if (iter == this->idxToElementMap.end()) {
    return nullptr;
  }
  return iter->second;
}

LLCStreamElementPtr LLCDynStream::getElemPanic(uint64_t elemIdx,
                                               const char *errMsg) const {
  auto elem = this->getElem(elemIdx);
  if (!elem) {
    LLC_S_PANIC(this->getDynStrandId(), "Failed to get LLCElem %lu for %s.",
                elemIdx, errMsg);
  }
  return elem;
}

void LLCDynStream::sample() {

  if (this->isTerminated() || !this->isRemoteConfigured()) {
    // The dynS is not configured yet or already terminated.
    return;
  }

  auto mlcSE = this->mlcController->getMLCStreamEngine();
  assert(mlcSE);
  if (auto mlcDynS = mlcSE->getStreamFromStrandId(this->getDynStrandId())) {
    mlcDynS->sample();
  }

  auto S = this->getStaticS();
  auto &statistic = S->statistic;
  auto &staticStats = statistic.getStaticStat();

  auto aliveElem = this->idxToElementMap.size();
  std::array<int, LLCStreamElement::State::NUM_STATE> elemState = {0};

  for (const auto &entry : this->idxToElementMap) {
    elemState[entry.second->getState()]++;
  }

  // LLC alive elements.
#define sample(sampler, v)                                                     \
  statistic.sampler.sample(v);                                                 \
  staticStats.sampler.sample(this->curCycle(), v);

  sample(remoteAliveElem, aliveElem);
  sample(remoteInitializedElem,
         elemState[LLCStreamElement::State::INITIALIZED]);
  sample(remoteReadyToIssueElem,
         elemState[LLCStreamElement::State::READY_TO_ISSUE]);
  sample(remoteIssuedElem, elemState[LLCStreamElement::State::ISSUED]);
  sample(remotePredicatedOffElem,
         elemState[LLCStreamElement::State::PREDICATED_OFF]);

  sample(remoteInflyCmp, this->incompleteComputations);
  sample(remoteInflyReq, this->inflyRequests);

#undef sample

  // Remote infly requests.
  for (auto indS : this->getIndStreams()) {
    indS->sample();
  }
}

} // namespace gem5
