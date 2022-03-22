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
#define DEBUG_TYPE LLCRubyStreamBase
#include "../stream_log.hh"

std::unordered_map<DynStrandId, LLCDynStream *, DynStrandIdHasher>
    LLCDynStream::GlobalLLCDynStreamMap;
std::unordered_map<NodeID, std::list<std::vector<LLCDynStream *>>>
    LLCDynStream::GlobalMLCToLLCDynStreamGroupMap;

// TODO: Support real flow control.
LLCDynStream::LLCDynStream(AbstractStreamAwareController *_mlcController,
                           AbstractStreamAwareController *_llcController,
                           CacheStreamConfigureDataPtr _configData)
    : mlcController(_mlcController), llcController(_llcController),
      maxInflyRequests(_llcController->getLLCStreamMaxInflyRequest()),
      configData(_configData), slicedStream(_configData),
      strandId(_configData->dynamicId, _configData->strandIdx,
               _configData->totalStrands),
      initializedCycle(_mlcController->curCycle()), creditedSliceIdx(0) {

  // Allocate the range builder.
  this->rangeBuilder = m5::make_unique<LLCStreamRangeBuilder>(
      this, this->configData->totalTripCount);

  // Remember the SendTo configs.
  for (auto &depEdge : this->configData->depEdges) {
    if (depEdge.type == CacheStreamConfigureData::DepEdge::Type::SendTo) {
      this->sendToConfigs.emplace_back(depEdge.data);
    }
  }

  // Remember the BaseOn configs.
  for (auto &baseEdge : this->configData->baseEdges) {
    this->baseOnConfigs.emplace_back(baseEdge.data.lock());
    this->reusedBaseElements.emplace_back(baseEdge.reuse);
    if (baseEdge.reuse <= 0) {
      LLC_S_PANIC(this->strandId, "Illegal Reuse Count %d on %s.",
                  baseEdge.reuse, baseEdge.data.lock()->dynamicId);
    }
    assert(this->baseOnConfigs.back() && "BaseStreamConfig already released?");
  }

  if (this->isPointerChase()) {
    // PointerChase allows at most one InflyRequest.
    this->maxInflyRequests = 1;
  }

  if (_configData->floatPlan.getFirstFloatElementIdx() > 0) {
    auto firstFloatElemIdx = _configData->floatPlan.getFirstFloatElementIdx();
    this->nextCommitElementIdx = firstFloatElemIdx;
    this->nextInitStrandElemIdx = firstFloatElemIdx;
    this->nextIssueElementIdx = firstFloatElemIdx;
    this->nextLoopBoundElementIdx = firstFloatElemIdx;
  }

  if (this->getStaticS()->isReduction() ||
      this->getStaticS()->isPointerChaseIndVar()) {
    // Initialize the first element for ReductionStream with the initial value.
    assert(this->isOneIterationBehind() &&
           "ReductionStream must be OneIterationBehind.");
    this->nextInitStrandElemIdx++;
  }

  LLC_S_DPRINTF_(LLCRubyStreamLife, this->getDynStrandId(), "Created.\n");
  assert(GlobalLLCDynStreamMap.emplace(this->getDynStrandId(), this).second);
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

bool LLCDynStream::hasTotalTripCount() const {
  if (this->baseStream) {
    return this->baseStream->hasTotalTripCount();
  }
  return this->slicedStream.hasTotalTripCount();
}

int64_t LLCDynStream::getTotalTripCount() const {
  if (this->baseStream) {
    return this->baseStream->getTotalTripCount();
  }
  return this->slicedStream.getTotalTripCount();
}

void LLCDynStream::setTotalTripCount(int64_t totalTripCount) {
  assert(!this->baseStream && "SetTotalTripCount for IndirectS.");
  this->slicedStream.setTotalTripCount(totalTripCount);
  this->rangeBuilder->receiveLoopBoundRet(totalTripCount);
  for (auto dynIS : this->indirectStreams) {
    dynIS->rangeBuilder->receiveLoopBoundRet(totalTripCount);
  }
}

MachineType LLCDynStream::getFloatMachineTypeAtElem(uint64_t elementIdx) const {
  if (this->isOneIterationBehind()) {
    if (elementIdx == 0) {
      LLC_S_PANIC(this->getDynStrandId(),
                  "Get FloatMachineType for Element 0 with OneIterBehind.");
    }
    elementIdx--;
  }
  return this->configData->floatPlan.getMachineTypeAtElem(elementIdx);
}

std::pair<Addr, MachineType>
LLCDynStream::peekNextInitVAddrAndMachineType() const {
  const auto &sliceId = this->slicedStream.peekNextSlice();
  auto startElemIdx = sliceId.getStartIdx();
  auto startElemMachineType = this->getFloatMachineTypeAtElem(startElemIdx);
  return std::make_pair(sliceId.vaddr, startElemMachineType);
}

const DynStreamSliceId &LLCDynStream::peekNextInitSliceId() const {
  return this->slicedStream.peekNextSlice();
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
    while (sliceIdx + 1 < this->creditedSliceIdx &&
           sliceIter != this->slices.end()) {
      ++sliceIter;
      ++sliceIdx;
    }
    if (sliceIter == this->slices.end()) {
      LLC_S_PANIC(this->getDynStrandId(),
                  "Missing Slice for RangeBuilder. Credited %llu.",
                  this->creditedSliceIdx);
    }
    auto &tailSlice = *sliceIter;
    auto tailElementIdx = tailSlice->getSliceId().getEndIdx();
    this->addNextRangeTailElementIdx(tailElementIdx);
  }
}

void LLCDynStream::addNextRangeTailElementIdx(uint64_t rangeTailElementIdx) {
  if (this->shouldRangeSync()) {
    this->rangeBuilder->pushNextRangeTailElementIdx(rangeTailElementIdx);
  }
  for (auto dynIS : this->getIndStreams()) {
    dynIS->rangeBuilder->pushNextRangeTailElementIdx(rangeTailElementIdx);
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
      LLC_S_DPRINTF(this->getDynStrandId(),
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
  for (auto IS : this->getIndStreams()) {
    IS->traceEvent(type);
  }
}

void LLCDynStream::sanityCheckStreamLife() {
  if (!Debug::LLCRubyStreamLife) {
    return;
  }
  bool failed = false;
  if (GlobalLLCDynStreamMap.size() > 4096) {
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
  assert(this->isNextSliceCredited() && "Next slice is not allocated yet.");
  if (auto slice = this->getNextAllocSlice()) {
    return slice->getSliceId().getStartIdx() >= this->getTotalTripCount();
  }

  LLC_S_PANIC(this->getDynStrandId(),
              "No Initialized Slice to check overflown.");
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
      for (auto elementIdx = sliceId.getStartIdx();
           elementIdx < sliceId.getEndIdx(); ++elementIdx) {
        auto element = this->getElementPanic(elementIdx, "AllocNextSlice.");
        if (element->vaddr != 0) {
          Addr paddr = 0;
          if (!this->translateToPAddr(element->vaddr, paddr)) {
            LLC_S_PANIC(this->getDynStrandId(),
                        "Translation fault on element %llu.", elementIdx);
          }
          if (!element->hasRangeBuilt()) {
            this->rangeBuilder->addElementAddress(elementIdx, element->vaddr,
                                                  paddr, element->size);
            element->setRangeBuilt();
          }
        }
      }
    }
    LLC_SLICE_DPRINTF(sliceId, "Allocated SliceIdx %llu VAddr %#x.\n",
                      this->nextAllocSliceIdx, slice->getSliceId().vaddr);
    this->invokeSliceAllocCallbacks(this->nextAllocSliceIdx);
    this->nextAllocSliceIdx++;
    // Slices allocated are now handled by LLCStreamEngine.
    this->slices.pop_front();

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

std::pair<Addr, MachineType>
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

void LLCDynStream::initDirectStreamSlicesUntil(uint64_t lastSliceIdx) {
  if (this->isIndirect()) {
    LLC_S_PANIC(this->getDynStrandId(),
                "InitDirectStreamSlice for IndirectStream.");
  }
  if (this->nextInitSliceIdx >= lastSliceIdx) {
    LLC_S_PANIC(this->getDynStrandId(),
                "Next DirectStreamSlice %llu already initialized.",
                lastSliceIdx);
  }
  while (this->nextInitSliceIdx < lastSliceIdx) {
    auto sliceId = this->initNextSlice();
    auto slice = std::make_shared<LLCStreamSlice>(this->getStaticS(), sliceId);

    // Register the elements.
    while (this->nextInitStrandElemIdx < sliceId.getEndIdx()) {
      this->initNextElement(
          this->slicedStream.getElementVAddr(this->nextInitStrandElemIdx));
    }

    // Remember the mapping from elements to slices.
    for (auto elementIdx = sliceId.getStartIdx(),
              endElementIdx = sliceId.getEndIdx();
         elementIdx < endElementIdx; ++elementIdx) {
      auto element = this->getElementPanic(elementIdx, "InitDirectSlice");
      element->addSlice(slice);
    }

    // Push into our queue.
    LLC_SLICE_DPRINTF(sliceId, "DirectStreamSlice Initialized.\n");
    this->slices.push_back(slice);
  }
}

void LLCDynStream::initNextElement(Addr vaddr) {
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
    if (this->idxToElementMap.size() >= 512) {
      int sliceNotReleasedElements = 0;
      for (const auto &entry : this->idxToElementMap) {
        const auto &element = entry.second;
        if (!element->areSlicesReleased()) {
          sliceNotReleasedElements++;
        }
      }
      if (sliceNotReleasedElements > 128) {
        for (const auto &entry : this->idxToElementMap) {
          const auto &element = entry.second;
          LLC_ELEMENT_HACK(
              element, "Element Overflow. Ready %d. AllSlicedReleased %d.\n",
              element->isReady(), element->areSlicesReleased());
          for (int i = 0, nSlices = element->getNumSlices(); i < nSlices; ++i) {
            const auto &slice = element->getSliceAt(i);
            Addr sliceVAddr = slice->getSliceId().vaddr;
            Addr slicePAddr = 0;
            this->translateToPAddr(sliceVAddr, slicePAddr);
            LLC_ELEMENT_HACK(element, "  Slice %s %s VAddr %#x PAddr %#x.",
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
                     "LLCElement Overflow, but MLCDynS Released?");
        }
        LLC_S_PANIC(this->getDynStrandId(), "Infly Elements Overflow %d.",
                    this->idxToElementMap.size());
      }
    }
  }

  auto size = this->getMemElementSize();
  auto element = std::make_shared<LLCStreamElement>(
      this->getStaticS(), this->mlcController, this->getDynStrandId(),
      strandElemIdx, vaddr, size, false /* isNDCElement */);
  this->idxToElementMap.emplace(strandElemIdx, element);
  this->nextInitStrandElemIdx++;

  for (auto i = 0; i < this->baseOnConfigs.size(); ++i) {

    /**
     * We populate the baseElements. If we are one iteration behind, we are
     * depending on the previous baseElements.
     * Adjust the BaseElemIdx to ReuseCount.
     * NOTE: Here we need to adjust BaseStreamElemIdx with ReuseCount.
     * NOTE: Be careful with StreamElemIdx and StrandElemIdx.
     */
    auto baseStreamElemIdx = streamElemIdx;
    if (this->isOneIterationBehind()) {
      assert(streamElemIdx > 0 &&
             "OneIterationBehind StreamElement begins at 1.");
      baseStreamElemIdx = streamElemIdx - 1;
    }

    auto &reusedBaseElem = this->reusedBaseElements.at(i);
    const auto reuseCount = reusedBaseElem.reuse;
    baseStreamElemIdx /= reuseCount;

    const auto &baseConfig = this->baseOnConfigs.at(i);
    auto baseStrandId =
        baseConfig->getStrandIdFromStreamElemIdx(baseStreamElemIdx);
    auto baseStrandElemIdx =
        baseConfig->getStrandElemIdxFromStreamElemIdx(baseStreamElemIdx);
    LLC_S_DPRINTF(this->getDynStrandId(),
                  "Add BaseElem (Reuse %d) from %s StreamElemIdx %llu "
                  "StrandElemIdx %llu.\n",
                  reuseCount, baseStrandId, baseStreamElemIdx,
                  baseStrandElemIdx);
    const auto &baseDynStreamId = baseConfig->dynamicId;
    auto baseS = baseConfig->stream;
    if (this->baseStream &&
        this->baseStream->getDynStreamId() == baseDynStreamId) {
      // This is from base stream. Should not have strands.
      assert(!this->configData->isSplitIntoStrands() && "Indirect Strand.");
      if (!this->baseStream->idxToElementMap.count(baseStreamElemIdx)) {
        LLC_ELEMENT_PANIC(element, "Missing base element from %s.",
                          this->baseStream->getDynStreamId());
      }
      element->baseElements.emplace_back(
          this->baseStream->idxToElementMap.at(baseStreamElemIdx));
    } else {
      /**
       * This is from another bank, we just create the element here.
       * If this is a remote affine stream, we directly get the ElementVAddr.
       * Otherwise, we set the vaddr to 0 and delay setting it in
       * recvStreamForwawrd().
       *
       * If the ReuseCount > 1 we check if we can borrow from allocated element.
       * If the ReuseCount is 1, we allocate the element.
       * Otherwise, and we have the same
       */
      LLCStreamElementPtr baseElem = nullptr;
      if (reusedBaseElem.reuse > 1 && reusedBaseElem.elem &&
          reusedBaseElem.streamElemIdx == baseStreamElemIdx) {
        // We can reuse.
        baseElem = reusedBaseElem.elem;
      } else {

        Addr baseElementVaddr = 0;
        auto linearBaseAddrGen =
            std::dynamic_pointer_cast<LinearAddrGenCallback>(
                baseConfig->addrGenCallback);
        if (linearBaseAddrGen) {
          baseElementVaddr =
              baseConfig->addrGenCallback
                  ->genAddr(baseStreamElemIdx, baseConfig->addrGenFormalParams,
                            getStreamValueFail)
                  .front();
        }

        // TODO: Need to translate between StrandElemIdx and StreamElemIdx.
        baseElem = std::make_shared<LLCStreamElement>(
            baseS, this->mlcController, baseStrandId, baseStrandElemIdx,
            baseElementVaddr, baseS->getMemElementSize(),
            false /* isNDCElement */);
      }
      element->baseElements.emplace_back(baseElem);
      // Update the ReusedStreamElem.
      reusedBaseElem.elem = baseElem;
      reusedBaseElem.streamElemIdx = baseStreamElemIdx;
    }
  }
  // Remember to add previous element as base element for reduction.
  if (this->getStaticS()->isReduction() ||
      this->getStaticS()->isPointerChaseIndVar()) {
    if (!this->lastReductionElement) {
      uint64_t firstElemIdx =
          this->configData->floatPlan.getFirstFloatElementIdx();
      // First time, just initialize the first element.
      this->lastReductionElement = std::make_shared<LLCStreamElement>(
          this->getStaticS(), this->mlcController, this->getDynStrandId(),
          firstElemIdx, 0, size, false /* isNDCElement */);
      this->lastReductionElement->setValue(
          this->configData->reductionInitValue);
      this->lastComputedReductionElementIdx = firstElemIdx;
    }

    auto baseStreamElemIdx = streamElemIdx;
    if (this->isOneIterationBehind()) {
      assert(streamElemIdx > 0 &&
             "OneIterationBehind StreamElement begins at 1.");
      baseStreamElemIdx = streamElemIdx - 1;
    }

    if (this->lastReductionElement->idx != baseStreamElemIdx) {
      LLC_S_PANIC(
          this->getDynStrandId(),
          "Missing previous Reduction LLCStreamElement %llu, Current %llu.",
          baseStreamElemIdx, this->lastReductionElement->idx);
    }
    /**
     * IndirectReductionStream is handled differently, as their computation
     * latency is charged at each indirect banks, but to keep things simple,
     * the real computation is still carried out serially.
     *
     * Therefore, we do not add PrevReductionElement as the base element for
     * IndirectReductionStream. Also, since the compute value is not truly
     * ready, we have to avoid any user of the reduction value.
     */
    if (this->isIndirectReduction()) {
      if (!this->configData->depEdges.empty()) {
        LLC_S_PANIC(this->getDynStrandId(),
                    "Dependence of IndirectReductionStream is not supported.");
      }
    } else {
      // Direct reduction.
      element->baseElements.emplace_back(this->lastReductionElement);
    }
    element->setPrevReductionElement(this->lastReductionElement);
    this->lastReductionElement = element;
  }

  /**
   * We call ElementInitCallback here.
   */
  auto elementInitCallbackIter = this->elementInitCallbacks.find(strandElemIdx);
  if (elementInitCallbackIter != this->elementInitCallbacks.end()) {
    for (auto &callback : elementInitCallbackIter->second) {
      callback(this->getDynStreamId(), strandElemIdx);
    }
    this->elementInitCallbacks.erase(elementInitCallbackIter);
  }

  // We allocate all indirect streams' element here with vaddr 0.
  for (auto &usedByS : this->getIndStreams()) {
    usedByS->initNextElement(0);
  }
}

bool LLCDynStream::isElementInitialized(uint64_t elementIdx) const {
  return elementIdx < this->nextInitStrandElemIdx;
}

void LLCDynStream::registerElementInitCallback(uint64_t elementIdx,
                                               ElementCallback callback) {
  if (this->isElementInitialized(elementIdx)) {
    LLC_S_PANIC(this->getDynStrandId(),
                "Register ElementInitCallback for InitializedElement %llu.",
                elementIdx);
  }
  this->elementInitCallbacks
      .emplace(std::piecewise_construct, std::forward_as_tuple(elementIdx),
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

bool LLCDynStream::isElementReleased(uint64_t elementIdx) const {
  if (this->idxToElementMap.empty()) {
    return elementIdx < this->nextInitStrandElemIdx;
  }
  return this->idxToElementMap.begin()->first > elementIdx;
}

uint64_t LLCDynStream::getNextUnreleasedElementIdx() const {
  return this->idxToElementMap.empty() ? this->getNextInitElementIdx()
                                       : this->idxToElementMap.begin()->first;
}

void LLCDynStream::eraseElement(uint64_t elementIdx) {
  auto iter = this->idxToElementMap.find(elementIdx);
  if (iter == this->idxToElementMap.end()) {
    LLC_S_PANIC(this->getDynStrandId(), "Failed to erase element %llu.",
                elementIdx);
  }
  LLC_ELEMENT_DPRINTF(iter->second, "Erased element.\n");
  this->getStaticS()->incrementOffloadedStepped();
  this->idxToElementMap.erase(iter);
}

void LLCDynStream::eraseElement(IdxToElementMapT::iterator elementIter) {
  LLC_ELEMENT_DPRINTF(elementIter->second, "Erased element.\n");
  this->getStaticS()->incrementOffloadedStepped();
  this->idxToElementMap.erase(elementIter);
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

void LLCDynStream::recvStreamForward(LLCStreamEngine *se,
                                     const uint64_t baseElementIdx,
                                     const DynStreamSliceId &sliceId,
                                     const DataBlock &dataBlk) {

  auto recvElementIdx = baseElementIdx;
  if (this->isOneIterationBehind()) {
    recvElementIdx++;
  }

  for (const auto &baseEdge : this->configData->baseEdges) {
    if (baseEdge.dynStreamId == sliceId.getDynStreamId() &&
        baseEdge.reuse != 1) {
      LLC_SLICE_DPRINTF(
          sliceId, "[Forward] Adjust RecvElementIdx %lu by Reuse %d = %lu.\n",
          recvElementIdx, baseEdge.reuse, recvElementIdx * baseEdge.reuse);
      recvElementIdx *= baseEdge.reuse;
      break;
    }
  }

  auto S = this->getStaticS();
  if (!this->idxToElementMap.count(recvElementIdx)) {
    LLC_SLICE_PANIC(
        sliceId,
        "Cannot find the receiver element %s %llu allocate from %llu.\n",
        this->getDynStreamId(), recvElementIdx, this->nextInitStrandElemIdx);
  }
  LLCStreamElementPtr recvElement = this->idxToElementMap.at(recvElementIdx);
  bool foundBaseElement = false;
  for (auto &baseElement : recvElement->baseElements) {
    if (baseElement->strandId == sliceId.getDynStrandId()) {
      /**
       * Found the base element to hold the data.
       * If the BaseStream is IndirectStream, we copy the ElementVaddr from the
       * slice.
       */
      if (baseElement->vaddr == 0) {
        baseElement->vaddr = sliceId.vaddr;
      }
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
    AbstractStreamAwareController *llcController) {
  this->setState(State::RUNNING);
  this->llcController = llcController;
  assert(this->prevConfiguredCycle == Cycles(0) && "Already RemoteConfigured.");
  this->prevConfiguredCycle = this->curCycle();
  auto &stats = this->getStaticS()->statistic;
  stats.numRemoteConfigure++;
  stats.numRemoteConfigureCycle +=
      this->prevConfiguredCycle - this->initializedCycle;
}

void LLCDynStream::migratingStart() {
  this->setState(State::MIGRATING);
  this->prevMigratedCycle = this->curCycle();
  this->traceEvent(::LLVM::TDG::StreamFloatEvent::MIGRATE_OUT);
  this->getStaticS()->se->numLLCMigrated++;

  auto &stats = this->getStaticS()->statistic;
  stats.numRemoteRunCycle +=
      this->prevMigratedCycle - this->prevConfiguredCycle;
}

void LLCDynStream::migratingDone(AbstractStreamAwareController *llcController) {

  /**
   * Notify the previous LLC SE that I have arrived.
   */
  assert(this->llcController && "Missing PrevLLCController after migration.");
  auto prevSE = this->llcController->getLLCStreamEngine();
  auto &prevMigrateController = prevSE->migrateController;
  prevMigrateController->migratedTo(this, llcController->getMachineID());

  this->llcController = llcController;
  this->setState(State::RUNNING);
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
    AbstractStreamAwareController *mlcController,
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
    //   uncuttedLLCDynSWithSmallestStatidId->setLoadBalanceValve();
  } else {
    std::sort(loadBalanceValueStreams.begin(), loadBalanceValueStreams.end(),
              [](const LLCDynStreamPtr &A, const LLCDynStreamPtr &B) -> bool {
                return A->getStaticId() < B->getStaticId();
              });
    loadBalanceValueStreams.front()->setLoadBalanceValve();
  }

  // Remember the allocated group.
  auto mlcNum = mlcController->getMachineID().getNum();
  auto &mlcGroups =
      GlobalMLCToLLCDynStreamGroupMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(mlcNum),
                   std::forward_as_tuple())
          .first->second;

  // Try to release old terminated groups.
  if (mlcGroups.size() == 100) {
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
    assert(mlcGroups.size() < 100 &&
           "Too many MLCGroups, streams are not released?");
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
              llcS->getDynStreamId());
      if (mlcNum != llcS->getDynStreamId().coreId) {
        panic("LLCStream %s released from wrong MLCGroup %d.",
              llcS->getDynStreamId(), mlcNum);
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

LLCDynStreamPtr
LLCDynStream::allocateLLCStream(AbstractStreamAwareController *mlcController,
                                CacheStreamConfigureDataPtr &config) {

  assert(config->initPAddrValid && "Initial paddr should be valid now.");
  auto initPAddr = config->initPAddr;

  // Get the MachineType of FirstFloatElementIdx.
  auto firstFloatElemIdx = config->floatPlan.getFirstFloatElementIdx();
  auto firstFloatElemMachineType =
      config->floatPlan.getMachineTypeAtElem(firstFloatElemIdx);

  auto llcMachineId =
      mlcController->mapAddressToLLCOrMem(initPAddr, firstFloatElemMachineType);
  auto llcController =
      AbstractStreamAwareController::getController(llcMachineId);

  // Create the stream.
  auto S = new LLCDynStream(mlcController, llcController, config);

  // Check if we have indirect streams.
  for (const auto &edge : config->depEdges) {
    if (edge.type != CacheStreamConfigureData::DepEdge::Type::UsedBy) {
      continue;
    }
    auto &ISConfig = edge.data;
    // Let's create an indirect stream.
    ISConfig->initCreditedIdx = config->initCreditedIdx;
    auto IS = new LLCDynStream(mlcController, llcController, ISConfig);
    IS->setBaseStream(S);
    for (const auto &ISDepEdge : ISConfig->depEdges) {
      if (ISDepEdge.type != CacheStreamConfigureData::DepEdge::UsedBy) {
        continue;
      }
      /**
       * So far we don't support Two-Level Indirect LLCStream, except:
       * 1. IndirectRedcutionStream.
       * 2. Two-Level IndirectStoreComputeStream.
       */
      auto ISDepS = ISDepEdge.data->stream;
      if (ISDepS->isReduction() || ISDepS->isStoreComputeStream()) {
        auto IIS =
            new LLCDynStream(mlcController, llcController, ISDepEdge.data);
        IIS->setBaseStream(IS);
        continue;
      }
      panic("Two-Level Indirect LLCStream is not supported: %s.",
            IS->getDynStreamId());
    }
  }

  // Create predicated stream information.
  assert(!config->isPredicated && "Base stream should never be predicated.");
  for (auto IS : S->getIndStreams()) {
    assert(!IS->isPredicated() && "Deprecated for now.");
    // if (IS->isPredicated()) {
    //   const auto &predSId = IS->getPredicateStreamId();
    //   auto predS = LLCDynStream::getLLCStream(predSId);
    //   assert(predS && "Failed to find predicate stream.");
    //   assert(predS != IS && "Self predication.");
    //   predS->predicatedStreams.insert(IS);
    //   IS->predicateStream = predS;
    // }
  }

  // Initialize the first slices.
  S->initDirectStreamSlicesUntil(config->initCreditedIdx);

  return S;
}

void LLCDynStream::setBaseStream(LLCDynStreamPtr baseS) {
  if (this->baseStream) {
    LLC_S_PANIC(this->getDynStrandId(), "Set multiple base LLCDynStream.");
  }
  this->baseStream = baseS;
  this->rootStream = baseS->rootStream ? baseS->rootStream : baseS;
  baseS->indirectStreams.push_back(this);
  this->rootStream->allIndirectStreams.push_back(this);
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
    if (type == MachineType::MachineType_L2Cache) {
      return "LLC";
    } else if (type == MachineType::MachineType_Directory) {
      return "MEM";
    }
  }
  return "XXX";
}

bool LLCDynStream::hasComputation() const {
  auto S = this->getStaticS();
  return S->isReduction() || S->isPointerChaseIndVar() ||
         S->isLoadComputeStream() || S->isStoreComputeStream() ||
         S->isUpdateStream() || S->isAtomicComputeStream();
}

StreamValue
LLCDynStream::computeStreamElementValue(const LLCStreamElementPtr &element) {

  auto S = element->S;
  const auto &config = this->configData;

  auto getBaseStreamValue = [&element](uint64_t baseStreamId) -> StreamValue {
    return element->getBaseStreamValue(baseStreamId);
  };
  auto getStreamValue = [&element](uint64_t baseStreamId) -> StreamValue {
    return element->getValueByStreamId(baseStreamId);
  };
  if (S->isReduction() || S->isPointerChaseIndVar()) {
    // This is a reduction stream.
    assert(element->idx > 0 &&
           "Reduction stream ElementIdx should start at 1.");

    // Perform the reduction.
    auto getBaseOrPrevReductionStreamValue =
        [&element](uint64_t baseStreamId) -> StreamValue {
      if (element->S->isCoalescedHere(baseStreamId)) {
        auto prevReductionElement = element->getPrevReductionElement();
        assert(prevReductionElement && "Missing prev reduction element.");
        assert(prevReductionElement->isReady() &&
               "Prev reduction element is not ready.");
        return prevReductionElement->getValueByStreamId(baseStreamId);
      }
      return element->getBaseStreamValue(baseStreamId);
    };

    Cycles latency = config->addrGenCallback->getEstimatedLatency();
    auto newReductionValue = config->addrGenCallback->genAddr(
        element->idx, config->addrGenFormalParams,
        getBaseOrPrevReductionStreamValue);
    if (Debug::LLCRubyStreamReduce) {
      std::stringstream ss;
      for (const auto &baseElement : element->baseElements) {
        ss << "\n  " << baseElement->strandId << '-' << baseElement->idx << ": "
           << baseElement->getValue(0, baseElement->size);
      }
      ss << "\n  -> " << element->strandId << '-' << element->idx << ": "
         << newReductionValue;
      LLC_ELEMENT_DPRINTF_(LLCRubyStreamReduce, element,
                           "[Latency %llu] Do reduction %s.\n", latency,
                           ss.str());
    }

    return newReductionValue;

  } else if (S->isStoreComputeStream()) {
    Cycles latency = config->storeCallback->getEstimatedLatency();
    auto params = convertFormalParamToParam(config->storeFormalParams,
                                            getBaseStreamValue);
    auto storeValue = config->storeCallback->invoke(params);

    LLC_ELEMENT_DPRINTF_(LLCRubyStreamStore, element,
                         "[Latency %llu] Compute StoreValue %s.\n", latency,
                         storeValue);
    return storeValue;

  } else if (S->isLoadComputeStream()) {
    // So far LoadComputeStream only takes loaded value as input.
    Cycles latency = config->loadCallback->getEstimatedLatency();
    auto params =
        convertFormalParamToParam(config->loadFormalParams, getStreamValue);
    auto loadComputeValue = config->loadCallback->invoke(params);

    LLC_ELEMENT_DPRINTF_(LLCRubyStreamStore, element,
                         "[Latency %llu] Compute LoadComputeValue %s.\n",
                         latency, loadComputeValue);
    return loadComputeValue;

  } else if (S->isUpdateStream()) {

    Cycles latency = config->storeCallback->getEstimatedLatency();
    auto getStreamValue = [&element](uint64_t streamId) -> StreamValue {
      return element->getBaseOrMyStreamValue(streamId);
    };
    auto params =
        convertFormalParamToParam(config->storeFormalParams, getStreamValue);
    auto storeValue = config->storeCallback->invoke(params);

    LLC_ELEMENT_DPRINTF_(LLCRubyStreamStore, element,
                         "[Latency %llu] Compute LoadComputeValue %s.\n",
                         latency, storeValue);
    return storeValue;

  } else if (S->isAtomicComputeStream()) {

    // So far Atomic are really computed at completeComputation.
    LLC_ELEMENT_DPRINTF_(LLCRubyStreamStore, element,
                         "[Latency %llu] Compute Dummy AtomicValue.\n",
                         S->getEstimatedComputationLatency());
    return StreamValue();

  } else {
    LLC_ELEMENT_PANIC(element, "No Computation for this stream.");
  }
}

void LLCDynStream::completeComputation(LLCStreamEngine *se,
                                       const LLCStreamElementPtr &element,
                                       const StreamValue &value) {
  auto S = this->getStaticS();
  element->doneComputation();
  /**
   * LoadCompute/Update store computed value in ComputedValue.
   * AtomicComputeStream has no computed value.
   * IndirectReductionStream separates compuation from charging the latency.
   */
  if (S->isLoadComputeStream() || S->isUpdateStream()) {
    element->setComputedValue(value);
  } else if (S->isAtomicComputeStream()) {

  } else if (!this->isIndirectReduction()) {
    element->setValue(value);
  }
  this->incompleteComputations--;
  assert(this->incompleteComputations >= 0 &&
         "Negative incomplete computations.");

  const auto seMachineID = se->controller->getMachineID();
  auto floatMachineType = this->getFloatMachineTypeAtElem(element->idx);
  if (seMachineID.getType() != floatMachineType) {
    LLC_ELEMENT_PANIC(element, "[CompleteCmp] Offload %s != SE MachineType %s.",
                      floatMachineType, seMachineID);
  }

  /**
   * For UpdateStream, we store here.
   */
  if (S->isUpdateStream()) {

    // Perform the operation.
    auto elementMemSize = S->getMemElementSize();
    auto elementCoreSize = S->getCoreElementSize();
    assert(elementCoreSize <= elementMemSize &&
           "CoreElementSize should not exceed MemElementSize.");
    auto elementVAddr = element->vaddr;

    Addr elementPAddr;
    assert(this->translateToPAddr(elementVAddr, elementPAddr) &&
           "Fault on vaddr of UpdateStream.");
    const auto lineSize = RubySystem::getBlockSizeBytes();

    assert(elementMemSize <= sizeof(value) && "UpdateStream size overflow.");
    for (int storedSize = 0; storedSize < elementMemSize;) {
      Addr vaddr = elementVAddr + storedSize;
      Addr paddr;
      if (!this->translateToPAddr(vaddr, paddr)) {
        LLC_ELEMENT_PANIC(element, "Fault on vaddr of UpdateStream.");
      }
      auto lineOffset = vaddr % lineSize;
      auto size = elementMemSize - storedSize;
      if (lineOffset + size > lineSize) {
        size = lineSize - lineOffset;
      }
      se->performStore(paddr, size, value.uint8Ptr(storedSize));
      storedSize += size;
    }
    LLC_ELEMENT_DPRINTF_(LLCRubyStreamStore, element,
                         "StreamUpdate done with value %s.\n", value);

  } else if (S->isAtomicComputeStream()) {

    // Ask the SE for post processing.
    assert(!S->isDirectMemStream() &&
           "Only IndirectAtomic will complete computation.");
    se->postProcessIndirectAtomicSlice(this, element);

  } else if (S->isReduction() || S->isPointerChaseIndVar()) {
    if (this->isIndirectReduction()) {
      /**
       * If this is IndirectReductionStream, perform the real computation.
       * Notice that here we charge zero latency, as we already charged it
       * when schedule the computation.
       */
      LLC_S_DPRINTF_(LLCRubyStreamReduce, this->getDynStrandId(),
                     "[IndirectReduction] Start real computation from "
                     "LastComputedElement %llu.\n",
                     this->lastComputedReductionElementIdx);
      while (this->lastComputedReductionElementIdx <
             this->getTotalTripCount()) {
        auto nextComputingElementIdx =
            this->lastComputedReductionElementIdx + 1;
        auto nextComputingElement = this->getElement(nextComputingElementIdx);
        if (!nextComputingElement) {
          LLC_S_DPRINTF_(
              LLCRubyStreamReduce, this->getDynStrandId(),
              "[IndirectReduction] Missing NextComputingElement %llu. "
              "Break.\n",
              nextComputingElementIdx);
          break;
        }
        if (!nextComputingElement->isComputationDone()) {
          LLC_S_DPRINTF_(LLCRubyStreamReduce, this->getDynStrandId(),
                         "[IndirectReduction] NextComputingElement %llu not "
                         "done. Break.\n",
                         nextComputingElementIdx);
          break;
        }
        // Really do the computation.
        LLC_S_DPRINTF_(LLCRubyStreamReduce, this->getDynStrandId(),
                       "[IndirectReduction] Really computed "
                       "NextComputingElement %llu.\n",
                       nextComputingElementIdx);
        auto result = this->computeStreamElementValue(nextComputingElement);
        nextComputingElement->setValue(result);
        this->lastComputedReductionElementIdx++;
      }
    } else {
      /**
       * If this is DirectReductionStream, check and schedule the next
       * element.
       */
      if (this->lastComputedReductionElementIdx + 1 != element->idx) {
        LLC_S_PANIC(this->getDynStrandId(),
                    "[DirectReduction] Reduction not in order.\n");
      }
      this->lastComputedReductionElementIdx++;
      if (this->idxToElementMap.count(element->idx + 1)) {
        auto &nextElement = this->idxToElementMap.at(element->idx + 1);
        if (nextElement->areBaseElementsReady()) {
          /**
           * We need to push the computation to the LLC SE at the correct
           * bank.
           */
          LLCStreamEngine *nextComputeSE = se;
          for (const auto &baseElement : nextElement->baseElements) {
            if (baseElement->strandId != this->baseStream->getDynStrandId()) {
              continue;
            }
            auto vaddr = baseElement->vaddr;
            if (vaddr != 0) {
              Addr paddr;
              assert(this->baseStream->translateToPAddr(vaddr, paddr) &&
                     "Failed to translate for NextReductionBaseElement.");
              auto llcMachineID = this->mlcController->mapAddressToLLCOrMem(
                  paddr, seMachineID.getType());
              nextComputeSE =
                  AbstractStreamAwareController::getController(llcMachineID)
                      ->getLLCStreamEngine();
            }
          }

          nextComputeSE->pushReadyComputation(nextElement);
        }
      }
    }

    /**
     * If the last reduction element is ready, we send this back to the core.
     */
    if (this->configData->finalValueNeededByCore &&
        this->lastComputedReductionElementIdx == this->getTotalTripCount()) {
      // This is the last reduction.
      auto finalReductionElement = this->getElementPanic(
          this->lastComputedReductionElementIdx, "Return FinalReductionValue.");

      DynStreamSliceId sliceId;
      sliceId.getDynStrandId() = this->getDynStrandId();
      sliceId.getStartIdx() = this->lastComputedReductionElementIdx;
      sliceId.getEndIdx() = this->lastComputedReductionElementIdx + 1;
      auto finalReductionValue =
          finalReductionElement->getValueByStreamId(S->staticId);

      Addr paddrLine = 0;
      int dataSize = sizeof(finalReductionValue);
      int payloadSize = S->getCoreElementSize();
      int lineOffset = 0;
      bool forceIdea = false;
      se->issueStreamDataToMLC(sliceId, paddrLine,
                               finalReductionValue.uint8Ptr(), dataSize,
                               payloadSize, lineOffset, forceIdea);
      LLC_ELEMENT_DPRINTF_(LLCRubyStreamReduce, finalReductionElement,
                           "Send back final reduction.\n");
    }

    /**
     * As a hack here, we release any older elements that are ready.
     */
    while (!this->idxToElementMap.empty()) {
      auto iter = this->idxToElementMap.begin();
      if (!iter->second->isReady()) {
        break;
      }
      this->eraseElement(iter);
    }
  }
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
        this->getElementPanic(elementIdx, "MarkCoreCommit for LLCElement");
    element->setCoreCommitted();
  }
}

void LLCDynStream::commitOneElement() {
  if (this->hasTotalTripCount() &&
      this->nextCommitElementIdx >= this->getTotalTripCount()) {
    LLC_S_PANIC(this->getDynStrandId(),
                "[Commit] Commit element %llu beyond TotalTripCount %llu.\n",
                this->nextCommitElementIdx, this->getTotalTripCount());
  }
  this->nextCommitElementIdx++;
  for (auto dynIS : this->getIndStreams()) {
    dynIS->commitOneElement();
  }
}

void LLCDynStream::markElementReadyToIssue(uint64_t elementIdx) {
  auto element =
      this->getElementPanic(elementIdx, "Mark IndirectElement ready to issue.");
  if (element->getState() != LLCStreamElement::State::INITIALIZED) {
    LLC_S_PANIC(this->getDynStrandId(),
                "IndirectElement in wrong state  %d to mark ready.",
                element->getState());
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
  auto getBaseStreamValue = [&element](uint64_t baseStreamId) -> StreamValue {
    return element->getBaseStreamValue(baseStreamId);
  };
  Addr elementVAddr =
      this->configData->addrGenCallback
          ->genAddr(elementIdx, this->configData->addrGenFormalParams,
                    getBaseStreamValue)
          .front();
  LLC_ELEMENT_DPRINTF(element, "Generate indirect vaddr %#x, size %d.\n",
                      elementVAddr, this->getMemElementSize());
  element->vaddr = elementVAddr;
  element->setState(LLCStreamElement::State::READY_TO_ISSUE);

  // Increment the counter in the base stream.
  this->numElementsReadyToIssue++;
  if (this->rootStream) {
    this->rootStream->numIndirectElementsReadyToIssue++;
  }
}

void LLCDynStream::markElementIssued(uint64_t elementIdx) {
  if (elementIdx != this->nextIssueElementIdx) {
    LLC_S_PANIC(this->getDynStrandId(),
                "IndirectElement should be issued in order.");
  }
  auto element =
      this->getElementPanic(elementIdx, "Mark IndirectElement issued.");
  if (element->getState() != LLCStreamElement::State::READY_TO_ISSUE) {
    LLC_S_PANIC(this->getDynStrandId(),
                "IndirectElement %llu not in ready state.", elementIdx);
  }
  /**
   * Notify the RangeBuilder.
   */
  if (this->shouldRangeSync()) {
    Addr paddr;
    if (!this->translateToPAddr(element->vaddr, paddr)) {
      LLC_S_PANIC(this->getDynStrandId(), "Fault on Issued element %llu.",
                  elementIdx);
    }
    this->rangeBuilder->addElementAddress(elementIdx, element->vaddr, paddr,
                                          element->size);
    element->setRangeBuilt();
  }
  element->setState(LLCStreamElement::State::ISSUED);
  assert(this->numElementsReadyToIssue > 0 &&
         "Underflow NumElementsReadyToIssue.");
  this->numElementsReadyToIssue--;
  this->nextIssueElementIdx++;
  if (this->rootStream) {
    assert(this->rootStream->numIndirectElementsReadyToIssue > 0 &&
           "Underflow NumIndirectElementsReadyToIssue.");
    this->rootStream->numIndirectElementsReadyToIssue--;
  }
}

LLCStreamElementPtr LLCDynStream::getFirstReadyToIssueElement() const {
  if (this->numElementsReadyToIssue == 0) {
    return nullptr;
  }
  auto element = this->getElementPanic(this->nextIssueElementIdx, __func__);
  switch (element->getState()) {
  default:
    LLC_S_PANIC(this->getDynStrandId(), "Element %llu with Invalid state %d.",
                element->idx, element->getState());
  case LLCStreamElement::State::INITIALIZED:
    // To guarantee in-order, return false here.
    return nullptr;
  case LLCStreamElement::State::READY_TO_ISSUE:
    return element;
  case LLCStreamElement::State::ISSUED:
    LLC_S_PANIC(this->getDynStrandId(), "NextIssueElement %llu already issued.",
                element->idx);
  }
}

bool LLCDynStream::shouldIssueBeforeCommit() const {
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
  if (!this->sendToConfigs.empty()) {
    return true;
  }
  auto S = this->getStaticS();
  if (S->isAtomicComputeStream()) {
    auto dynS = this->getCoreDynS();
    if (S->staticId == 81) {
      LLC_S_DPRINTF(this->getDynStrandId(),
                    "[IssueBeforeCommitTest] dynS %d.\n", dynS != nullptr);
    }
    if (dynS && !dynS->shouldCoreSEIssue()) {
      return false;
    }
  }
  return true;
}

bool LLCDynStream::shouldIssueAfterCommit() const {
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

void LLCDynStream::evaluateLoopBound(LLCStreamEngine *se) {
  if (!this->hasLoopBound()) {
    return;
  }
  if (this->loopBoundBrokenOut) {
    return;
  }

  auto element = this->getElement(this->nextLoopBoundElementIdx);
  if (!element) {
    if (this->isElementReleased(this->nextLoopBoundElementIdx)) {
      LLC_S_PANIC(this->getDynStrandId(),
                  "[LLCLoopBound] NextLoopBoundElement %llu released.",
                  this->nextLoopBoundElementIdx);
    } else {
      // Not allocated yet.
      return;
    }
  }
  if (!element->isReady()) {
    LLC_S_DPRINTF_(LLCStreamLoopBound, this->getDynStrandId(),
                   "[LLCLoopBound] NextLoopBoundElement %llu not ready.\n",
                   this->nextLoopBoundElementIdx);
    return;
  }

  auto getStreamValue = [&element](uint64_t streamId) -> StreamValue {
    return element->getValueByStreamId(streamId);
  };
  auto loopBoundActualParams = convertFormalParamToParam(
      this->configData->loopBoundFormalParams, getStreamValue);
  auto loopBoundRet =
      this->configData->loopBoundCallback->invoke(loopBoundActualParams)
          .front();

  this->nextLoopBoundElementIdx++;
  if (loopBoundRet == this->configData->loopBoundRet) {
    /**
     * We should break.
     * So far we just magically set TotalTripCount for Core/MLC/LLC.
     * TODO: Handle this in a more realistic way.
     */
    LLC_S_DPRINTF_(LLCStreamLoopBound, this->getDynStrandId(),
                   "[LLCLoopBound] Break (%d == %d) TripCount %llu.\n",
                   loopBoundRet, this->configData->loopBoundRet,
                   this->nextLoopBoundElementIdx);

    Addr loopBoundBrokenPAddr;
    if (!this->translateToPAddr(element->vaddr, loopBoundBrokenPAddr)) {
      LLC_S_PANIC(this->getDynStrandId(),
                  "[LLCLoopBound] BrokenOut Element VAddr %#x Faulted.",
                  element->vaddr);
    }

    this->setTotalTripCount(this->nextLoopBoundElementIdx);

    // Also set the MLCStream.
    se->sendOffloadedLoopBoundRetToMLC(this, this->nextLoopBoundElementIdx,
                                       loopBoundBrokenPAddr);

    // Also set the core.
    this->getStaticS()->getSE()->receiveOffloadedLoopBoundRet(
        this->getDynStreamId(), this->nextLoopBoundElementIdx,
        true /* BrokenOut */);

    this->loopBoundBrokenOut = true;
    this->loopBoundBrokenPAddr = loopBoundBrokenPAddr;

  } else {
    /**
     * We should continue.
     * Currently the core would just be blocking to wait wait for the
     * the final results. However, the current implementation still
     * steps through all the elements. To avoid a burst of stepping
     * after we determine the TotalTripCount, here we also notify
     * the core SE that you can step this "fake" element.
     */
    LLC_S_DPRINTF_(LLCStreamLoopBound, this->getDynStrandId(),
                   "[LLCLoopBound] Continue (%d != %d) Element %llu.\n",
                   loopBoundRet, this->configData->loopBoundRet,
                   this->nextLoopBoundElementIdx);
    this->getStaticS()->getSE()->receiveOffloadedLoopBoundRet(
        this->getDynStreamId(), this->nextLoopBoundElementIdx,
        false /* BrokenOut */);
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

  auto nextLoopBoundElemIdx = this->getNextLoopBoundElemIdx();
  if (nextLoopBoundElemIdx < sliceId.getStartIdx()) {
    // We are still waiting for some previous slice, not done.
    return false;
  }
  if (nextLoopBoundElemIdx < sliceId.getEndIdx()) {
    /**
     * It is possible that we need to evaluate LoopBound for this slice.
     * Due to multi-line elements, we check that this slice completes the
     * element, i.e. it contains the last byte of this element.
     * In such case, we evalute the LoopBound and not release the slice.
     */
    auto element = this->getElementPanic(nextLoopBoundElemIdx,
                                         "Check element for LoopBound.");
    int sliceOffset;
    int elementOffset;
    int overlapSize = element->computeOverlap(sliceId.vaddr, sliceId.getSize(),
                                              sliceOffset, elementOffset);
    assert(overlapSize > 0 && "Empty overlap.");
    if (elementOffset + overlapSize == element->size) {
      // This slice contains the last element byte.
      return false;
    }
  }
  return true;
}

LLCStreamElementPtr LLCDynStream::getElement(uint64_t elementIdx) const {
  auto iter = this->idxToElementMap.find(elementIdx);
  if (iter == this->idxToElementMap.end()) {
    return nullptr;
  }
  return iter->second;
}

LLCStreamElementPtr LLCDynStream::getElementPanic(uint64_t elementIdx,
                                                  const char *errMsg) const {
  auto element = this->getElement(elementIdx);
  if (!element) {
    LLC_S_PANIC(this->getDynStrandId(),
                "Failed to get LLCStreamElement %llu for %s.", elementIdx,
                errMsg);
  }
  return element;
}