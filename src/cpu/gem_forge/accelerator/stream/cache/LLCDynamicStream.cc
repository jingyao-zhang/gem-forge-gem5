#include "LLCDynamicStream.hh"

#include "cpu/gem_forge/accelerator/stream/coalesced_stream.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "debug/LLCRubyStreamBase.hh"
#include "debug/LLCRubyStreamLife.hh"
#define DEBUG_TYPE LLCRubyStreamBase
#include "../stream_log.hh"

std::unordered_map<DynamicStreamId, LLCDynamicStream *, DynamicStreamIdHasher>
    LLCDynamicStream::GlobalLLCDynamicStreamMap;

LLCStreamElement::LLCStreamElement(LLCDynamicStream *_dynS, uint64_t _idx,
                                   Addr _vaddr, int _size)
    : dynS(_dynS), idx(_idx), vaddr(_vaddr), size(_size), readyBytes(0) {
  if (this->size > MAX_SIZE) {
    panic("LLCStreamElement size overflow %d, %s.\n", this->size,
          this->dynS->getDynamicStreamId().streamName);
  }
  this->data.fill(0);
}

uint64_t LLCStreamElement::getData(uint64_t streamId) const {
  assert(this->isReady());
  auto S = this->dynS->getStaticStream();
  int32_t offset = 0;
  int size = this->size;
  if (auto CS = dynamic_cast<CoalescedStream *>(S)) {
    // Handle offset for coalesced stream.
    CS->getCoalescedOffsetAndSize(streamId, offset, size);
  }
  assert(size <= sizeof(uint64_t) && "ElementSize overflow.");
  assert(offset + size <= this->size && "Size overflow.");
  return GemForgeUtils::rebuildData(this->data.data() + offset, size);
}

// TODO: Support real flow control.
LLCDynamicStream::LLCDynamicStream(AbstractStreamAwareController *_controller,
                                   CacheStreamConfigureData *_configData)
    : configData(*_configData),
      slicedStream(_configData, true /* coalesceContinuousElements */),
      maxInflyRequests(_controller->getLLCStreamMaxInflyRequest()),
      configureCycle(_controller->curCycle()), controller(_controller),
      sliceIdx(0), allocatedSliceIdx(_configData->initAllocatedIdx),
      inflyRequests(0) {
  if (this->configData.isPointerChase) {
    // Pointer chase stream can only have at most one base requests waiting for
    // data.
    this->maxInflyRequests = 1;
  }
  if (this->getStaticStream()->isReduction()) {
    // Copy the initial reduction value.
    this->reductionValue = this->configData.reductionInitValue;
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
  return this->configData.totalTripCount != -1;
}

uint64_t LLCDynamicStream::getTotalTripCount() const {
  if (this->baseStream) {
    return this->baseStream->getTotalTripCount();
  }
  return this->configData.totalTripCount;
}

Addr LLCDynamicStream::peekVAddr() {
  return this->slicedStream.peekNextSlice().vaddr;
}

Addr LLCDynamicStream::getVAddr(uint64_t sliceIdx) const {
  panic("getVAddr is deprecated.\n");
  return 0;
}

bool LLCDynamicStream::translateToPAddr(Addr vaddr, Addr &paddr) const {
  // ! Do something reasonable here to translate the vaddr.
  auto cpuDelegator = this->configData.stream->getCPUDelegator();
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
  const auto *dynS =
      this->configData.stream->getDynamicStream(this->configData.dynamicId);
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
      LLC_S_DPRINTF(this->configData.dynamicId,
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
        if (IS->isMerged() &&
            IS->getStreamType() == ::LLVM::TDG::StreamInfo_Type_ST) {
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
  }

  this->shouldUpdateIssueClearCycleInitialized = true;
  return this->shouldUpdateIssueClearCycleMemorized;
}

void LLCDynamicStream::traceEvent(
    const ::LLVM::TDG::StreamFloatEvent::StreamFloatEventType &type) {
  auto &floatTracer = this->getStaticStream()->floatTracer;
  auto curCycle = this->controller->curCycle();
  auto llcBank = this->controller->getMachineID().num;
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
  auto curCycle = this->controller->curCycle();
  bool failed = false;
  if (GlobalLLCDynamicStreamMap.size() > 32) {
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
          this->controller->curCycle());
  assert(false);
}