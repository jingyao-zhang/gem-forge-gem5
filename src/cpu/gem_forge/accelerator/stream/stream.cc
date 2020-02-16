#include "stream.hh"
#include "insts.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"

// #include "base/misc.hh""
#include "base/trace.hh"
#include "proto/protoio.hh"

#include "debug/StreamBase.hh"
#define DEBUG_TYPE StreamBase
#include "stream_log.hh"

#define STREAM_DPRINTF(format, args...) S_DPRINTF(this, format, ##args)

#define STREAM_HACK(format, args...)                                           \
  hack("Stream %s: " format, this->getStreamName().c_str(), ##args)

#define STREAM_ENTRY_HACK(entry, format, args...)                              \
  STREAM_HACK("Entry (%lu, %lu): " format, (entry).idx.streamInstance,         \
              (entry).idx.entryIdx, ##args)

#define STREAM_PANIC(format, args...)                                          \
  {                                                                            \
    this->dump();                                                              \
    panic("Stream %s: " format, this->getStreamName().c_str(), ##args);        \
  }

#define STREAM_ENTRY_PANIC(entry, format, args...)                             \
  STREAM_PANIC("Entry (%lu, %lu): " format, (entry).idx.streamInstance,        \
               (entry).idx.entryIdx, ##args)

Stream::Stream(const StreamArguments &args)
    : FIFOIdx(DynamicStreamId(args.cpuDelegator->cpuId(), args.staticId,
                              0 /*StreamInstance*/)),
      staticId(args.staticId), streamName(args.name), cpu(args.cpu),
      cpuDelegator(args.cpuDelegator), se(args.se) {

  this->configured = false;
  this->allocSize = 0;
  this->stepSize = 0;
  this->maxSize = args.maxSize;
  this->stepRootStream = nullptr;
  this->lateFetchCount = 0;
  this->streamRegion = args.streamRegion;

  // The name field in the dynamic id has to be set here after we initialize
  // streamName.
  this->FIFOIdx.streamId.streamName = this->streamName.c_str();
}

Stream::~Stream() {}

void Stream::dumpStreamStats(std::ostream &os) const {
  os << this->getStreamName() << '\n';
  this->statistic.dump(os);
}

bool Stream::isMemStream() const {
  return this->getStreamType() == "load" || this->getStreamType() == "store";
}

void Stream::addBaseStream(Stream *baseStream) {
  if (baseStream == this) {
    STREAM_PANIC("Base stream should not be self.");
  }
  this->baseStreams.insert(baseStream);
  baseStream->dependentStreams.insert(this);
}

void Stream::addBackBaseStream(Stream *backBaseStream) {
  if (backBaseStream == this) {
    STREAM_PANIC("Base stream should not be self.");
  }
  this->backBaseStreams.insert(backBaseStream);
  backBaseStream->backDependentStreams.insert(this);
}

void Stream::addBaseStepStream(Stream *baseStepStream) {
  if (baseStepStream == this) {
    STREAM_PANIC("Base stream should not be self.");
  }
  this->baseStepStreams.insert(baseStepStream);
  baseStepStream->dependentStepStreams.insert(this);
  if (baseStepStream->isStepRoot()) {
    this->baseStepRootStreams.insert(baseStepStream);
  } else {
    for (auto stepRoot : baseStepStream->baseStepRootStreams) {
      this->baseStepRootStreams.insert(stepRoot);
    }
  }
}

void Stream::registerStepDependentStreamToRoot(Stream *newStepDependentStream) {
  if (!this->isStepRoot()) {
    STREAM_PANIC("Try to register step instruction to non-root stream.");
  }
  for (auto &stepStream : this->stepStreamList) {
    if (stepStream == newStepDependentStream) {
      STREAM_PANIC(
          "The new step dependent stream has already been registered.");
    }
  }
  this->stepStreamList.emplace_back(newStepDependentStream);
}

void Stream::dispatchStreamConfig(uint64_t seqNum, ThreadContext *tc) {
  // Remember to old index for rewinding.
  auto prevFIFOIdx = this->FIFOIdx;
  // Create new index.
  this->FIFOIdx.newInstance(seqNum);
  // Allocate the new DynamicStream.
  this->dynamicStreams.emplace_back(this->FIFOIdx.streamId, seqNum,
                                    cpuDelegator->curCycle(), tc, prevFIFOIdx,
                                    this->se);
}

void Stream::executeStreamConfig(uint64_t seqNum,
                                 const std::vector<uint64_t> *inputVec) {
  auto &dynStream = this->getDynamicStream(seqNum);
  assert(!dynStream.configExecuted && "StreamConfig already executed.");
  dynStream.configExecuted = true;

  /**
   * We intercept the constant update value here.
   */
  std::vector<uint64_t> inputVecCopy(*inputVec);
  this->extractExtraInputValues(dynStream, inputVecCopy);
  this->setupAddrGen(dynStream, &inputVecCopy);
}

void Stream::rewindStreamConfig(uint64_t seqNum) {
  // Rewind must happen in reverse order.
  assert(!this->dynamicStreams.empty() &&
         "Missing DynamicStream when rewinding StreamConfig.");
  auto &dynStream = this->dynamicStreams.back();
  assert(dynStream.configSeqNum == seqNum && "Mismatch configSeqNum.");

  // Check if we have offloaded this.
  if (dynStream.offloadedToCache) {
    // ! Jesus, donot know how to rewind an offloaded stream yet.
    panic("Don't support rewind offloaded stream.");
  }

  /**
   * Get rid of any unstepped elements.
   */
  while (this->allocSize > this->stepSize) {
    this->se->releaseElementUnstepped(this);
  }

  /**
   * Reset the FIFOIdx.
   * ! This is required as StreamEnd does not remember it.
   */
  this->FIFOIdx = dynStream.prevFIFOIdx;

  // Get rid of the dynamicStream.
  this->dynamicStreams.pop_back();

  assert(this->allocSize == this->stepSize &&
         "Unstepped elements when rewind StreamConfig.");
  this->statistic.numMisConfigured++;
  this->configured = false;
}

bool Stream::isStreamConfigureExecuted(uint64_t seqNum) {
  auto &dynStream = this->getDynamicStream(seqNum);
  return dynStream.configExecuted;
}

void Stream::dispatchStreamEnd(uint64_t seqNum) {
  assert(this->configured && "Stream should be configured.");
  auto &dynS = this->getLastDynamicStream();
  assert(!dynS.endDispatched && "Already ended.");
  assert(dynS.configSeqNum < seqNum && "End before configure.");
  dynS.endDispatched = true;
  this->configured = false;
}

void Stream::rewindStreamEnd(uint64_t seqNum) {
  assert(!this->configured && "Stream should not be configured.");
  auto &dynS = this->getLastDynamicStream();
  assert(dynS.endDispatched && "Not ended.");
  assert(dynS.configSeqNum < seqNum && "End before configure.");
  dynS.endDispatched = false;
  this->configured = true;
}

void Stream::commitStreamEnd(uint64_t seqNum) {
  assert(!this->dynamicStreams.empty() &&
         "Empty dynamicStreams for StreamEnd.");
  auto &dynS = this->dynamicStreams.front();
  assert(dynS.configSeqNum < seqNum && "End before config.");
  assert(dynS.configExecuted && "End before config executed.");

  /**
   * We need to release all unstepped elements.
   */
  assert(dynS.stepSize == 0 && "Stepped but unreleased element.");
  assert(dynS.allocSize == 0 && "Unreleased element.");

  // Update stats of cycles.
  auto endCycle = cpuDelegator->curCycle();
  this->statistic.numCycle += endCycle - dynS.configCycle;

  this->dynamicStreams.pop_front();
  if (!this->dynamicStreams.empty()) {
    // There is another config inst waiting.
    assert(this->dynamicStreams.front().configSeqNum > seqNum &&
           "Next StreamConfig not younger than previous StreamEnd.");
  }
}

DynamicStream &Stream::getDynamicStream(uint64_t seqNum) {
  for (auto &dynStream : this->dynamicStreams) {
    if (dynStream.configSeqNum == seqNum) {
      return dynStream;
    }
  }
  panic("Failed to find DynamicStream %llu.\n", seqNum);
}

DynamicStream *Stream::getDynamicStream(const DynamicStreamId &dynId) {
  for (auto &dynStream : this->dynamicStreams) {
    if (dynStream.dynamicStreamId == dynId) {
      return &dynStream;
    }
  }
  return nullptr;
}

void Stream::setupLinearAddrFunc(DynamicStream &dynStream,
                                 const std::vector<uint64_t> *inputVec,
                                 const LLVM::TDG::StreamInfo &info) {
  assert(inputVec && "Missing InputVec.");
  const auto &staticInfo = info.static_info();
  const auto &pattern = staticInfo.iv_pattern();
  assert(pattern.val_pattern() == ::LLVM::TDG::StreamValuePattern::LINEAR);
  /**
   * LINEAR pattern has 2n or (2n+1) parameters, where n is the difference of
   * loop level between ConfigureLoop and InnerMostLoop. It has the following
   * format, starting from InnerMostLoop. Stride0, [BackEdgeCount[i], Stride[i +
   * 1]]*, [BackEdgeCount[n]], Start We will add 1 to BackEdgeCount to get the
   * TripCount.
   */
  assert(pattern.params_size() >= 2 && "Number of parameters must be >= 2.");
  auto &formalParams = dynStream.addrGenFormalParams;
  auto inputIdx = 0;
  for (const auto &param : pattern.params()) {
    formalParams.emplace_back();
    auto &formalParam = formalParams.back();
    formalParam.isInvariant = true;
    if (param.is_static()) {
      // This param comes from the Configuration.
      // hack("Find valid param #%d, val %llu.\n", formalParams.size(),
      //      param.param());
      formalParam.param.invariant = param.value();
    } else {
      // This should be an input.
      assert(inputIdx < inputVec->size() && "Overflow of inputVec.");
      // hack("Find input param #%d, val %llu.\n", formalParams.size(),
      //      inputVec->at(inputIdx));
      formalParam.param.invariant = inputVec->at(inputIdx);
      inputIdx++;
    }
  }

  assert(inputIdx == inputVec->size() && "Unused input value.");

  /**
   * We have to process the params to compute TotalTripCount for each nested
   * loop.
   * TripCount[i] = BackEdgeCount[i] + 1;
   * TotalTripCount[i] = TotalTripCount[i - 1] * TripCount[i];
   */
  STREAM_DPRINTF("Setup LinearAddrGenCallback with Input params --------\n");
  for (auto param : *inputVec) {
    STREAM_DPRINTF("%llu\n", param);
  }
  STREAM_DPRINTF("Setup LinearAddrGenCallback with params --------\n");
  for (auto param : formalParams) {
    STREAM_DPRINTF("%llu\n", param.param.invariant);
  }

  for (auto idx = 1; idx < formalParams.size() - 1; idx += 2) {
    auto &formalParam = formalParams.at(idx);
    // BackEdgeCount.
    auto backEdgeCount = formalParam.param.invariant;
    // TripCount.
    auto tripCount = backEdgeCount + 1;
    // TotalTripCount.
    auto totalTripCount =
        (idx == 1) ? (tripCount)
                   : (tripCount * formalParams.at(idx - 2).param.invariant);
    formalParam.param.invariant = totalTripCount;
  }

  STREAM_DPRINTF("Finalize LinearAddrGenCallback with params --------\n");
  for (auto param : formalParams) {
    STREAM_DPRINTF("%llu\n", param.param.invariant);
  }

  // Set the callback.
  dynStream.addrGenCallback =
      std::unique_ptr<LinearAddrGenCallback>(new LinearAddrGenCallback());

  // Update the totalTripCount to the dynamic stream if possible.
  if (formalParams.size() % 2 == 1) {
    dynStream.totalTripCount =
        formalParams.at(formalParams.size() - 2).param.invariant;
  }
}

void Stream::setupFuncAddrFunc(DynamicStream &dynStream,
                               const std::vector<uint64_t> *inputVec,
                               const LLVM::TDG::StreamInfo &info) {

  const auto &addrFuncInfo = info.addr_func_info();
  assert(addrFuncInfo.name() != "" && "Missing AddrFuncInfo.");
  auto &formalParams = dynStream.addrGenFormalParams;
  auto usedInputs =
      this->setupFormalParams(inputVec, addrFuncInfo, formalParams);
  assert(usedInputs == inputVec->size() && "Underflow of inputVec.");
  // Set the callback.
  auto addrExecFunc =
      std::make_shared<TheISA::ExecFunc>(dynStream.tc, addrFuncInfo);
  dynStream.addrGenCallback =
      std::make_shared<FuncAddrGenCallback>(addrExecFunc);
}

int Stream::setupFormalParams(const std::vector<uint64_t> *inputVec,
                              const LLVM::TDG::ExecFuncInfo &info,
                              DynamicStreamFormalParamV &formalParams) {
  assert(info.name() != "" && "Missing AddrFuncInfo.");
  int inputIdx = 0;
  for (const auto &arg : info.args()) {
    if (arg.is_stream()) {
      // This is a stream input.
      // hack("Find stream input param #%d id %llu.\n",
      // formalParams.size(),
      //      arg.stream_id());
      formalParams.emplace_back();
      auto &formalParam = formalParams.back();
      formalParam.isInvariant = false;
      formalParam.param.baseStreamId = arg.stream_id();
    } else {
      assert(inputIdx < inputVec->size() && "Overflow of inputVec.");
      // hack("Find invariant param #%d, val %llu.\n",
      // formalParams.size(),
      //      inputVec->at(inputIdx));
      formalParams.emplace_back();
      auto &formalParam = formalParams.back();
      formalParam.isInvariant = true;
      formalParam.param.invariant = inputVec->at(inputIdx);
      inputIdx++;
    }
  }
  return inputIdx;
}

/**
 * Extract extra input values from the inputVec. May modify inputVec.
 */
void Stream::extractExtraInputValues(DynamicStream &dynS,
                                     std::vector<uint64_t> &inputVec) {

  /**
   * If this is a load stream upgraded to update stream.
   */
  if (this->hasUpgradedToUpdate()) {
    const auto &updateParam = this->getConstUpdateParam();
    if (updateParam.is_static()) {
      // Static input.
      dynS.constUpdateValue = updateParam.value();
    } else {
      // Dynamic input.
      assert(!inputVec.empty() && "Missing dynamic const update value.");
      dynS.constUpdateValue = inputVec.front();
      inputVec.erase(inputVec.begin());
    }
  }
  /**
   * If this is a merged store stream, we only support const store so far.
   */
  if (this->isMerged()) {
    const auto &type = this->getStreamType();
    if (type == "store") {
      const auto &updateParam = this->getConstUpdateParam();
      if (updateParam.is_static()) {
        // Static input.
        dynS.constUpdateValue = updateParam.value();
      } else {
        // Dynamic input.
        assert(!inputVec.empty() && "Missing dynamic const update value.");
        dynS.constUpdateValue = inputVec.front();
        inputVec.erase(inputVec.begin());
      }
    }
  }
  /**
   * If this has is load stream with merged predicated stream, check for
   * any inputs for the predication function.
   */
  const auto &mergedPredicatedStreams = this->getMergedPredicatedStreams();
  if (mergedPredicatedStreams.size() > 0) {
    const auto &predFuncInfo = this->getPredicateFuncInfo();
    dynS.predCallback =
        std::make_shared<TheISA::ExecFunc>(dynS.tc, predFuncInfo);
    auto &predFormalParams = dynS.predFormalParams;
    auto usedInputs =
        this->setupFormalParams(&inputVec, predFuncInfo, predFormalParams);
    // Consume these inputs.
    inputVec.erase(inputVec.begin(), inputVec.begin() + usedInputs);
  }
  /**
   * If this is a reduction stream, check for the initial value.
   */
  if (this->isReduction()) {
    assert(!inputVec.empty() && "Missing initial value for reduction stream.");
    dynS.initialValue = inputVec.front();
    inputVec.erase(inputVec.begin());
  }
}

CacheStreamConfigureData *
Stream::allocateCacheConfigureData(uint64_t configSeqNum, bool isIndirect) {
  auto &dynStream = this->getDynamicStream(configSeqNum);
  auto configData = new CacheStreamConfigureData(
      this, dynStream.dynamicStreamId, this->getElementSize(),
      dynStream.addrGenFormalParams, dynStream.addrGenCallback);

  // Set the totalTripCount.
  configData->totalTripCount = dynStream.totalTripCount;

  // Set the ConstUpdateValue.
  configData->constUpdateValue = dynStream.constUpdateValue;

  // Set the predication function.
  configData->predFormalParams = dynStream.predFormalParams;
  configData->predCallback = dynStream.predCallback;

  // Set the reduction information.
  configData->reductionInitValue = dynStream.initialValue;

  // Set the initial vaddr if this is not indirect stream.
  if (!isIndirect) {
    configData->initVAddr = dynStream.addrGenCallback->genAddr(
        0, dynStream.addrGenFormalParams, getStreamValueFail);
    // Remember to make line address.
    configData->initVAddr -=
        configData->initVAddr % this->cpuDelegator->cacheLineSize();

    Addr initPAddr;
    if (this->cpuDelegator->translateVAddrOracle(configData->initVAddr,
                                                 initPAddr)) {
      configData->initPAddr = initPAddr;
      configData->initPAddrValid = true;
    } else {
      /**
       * In case of faulted initVAddr, we simply set the initPAddr to 0
       * and mark it invalid. Later the MLC StreamEngine will pick up
       * a physical address that maps to the closes LLC bank and let the
       * stream spin there until we have a valid address.
       */
      configData->initPAddr = 0;
      configData->initPAddrValid = false;
    }
  }

  return configData;
}

bool Stream::isDirectMemStream() const {
  if (!this->isMemStream()) {
    return false;
  }
  // So far only only one base stream of phi type of the same loop level.
  for (auto baseS : this->baseStreams) {
    if (baseS->getLoopLevel() != this->getLoopLevel()) {
      // Ignore streams from different loop level.
      continue;
    }
    if (baseS->getStreamType() != "phi") {
      return false;
    }
    if (!baseS->backBaseStreams.empty()) {
      return false;
    }
  }
  return true;
}

bool Stream::isDirectLoadStream() const {
  if (this->getStreamType() != "load") {
    return false;
  }
  return this->isDirectMemStream();
}

void Stream::allocateElement(StreamElement *newElement) {

  assert(this->configured &&
         "Stream should be configured to allocate element.");
  this->statistic.numAllocated++;
  newElement->stream = this;

  /**
   * Append this new element to the last dynamic stream.
   */
  auto &dynS = this->getLastDynamicStream();
  DYN_S_DPRINTF(dynS.dynamicStreamId, "Try to allocate element.\n");

  /**
   * next() is called after assign to make sure
   * entryIdx starts from 0.
   */
  newElement->FIFOIdx = dynS.FIFOIdx;
  newElement->isCacheBlockedValue = this->isMemStream();
  dynS.FIFOIdx.next();

  if (dynS.totalTripCount > 0 &&
      newElement->FIFOIdx.entryIdx >= dynS.totalTripCount + 1) {
    S_PANIC(
        this,
        "Allocate beyond totalTripCount %lu, allocSize %lu, entryIdx %lu.\n",
        dynS.totalTripCount, this->getAllocSize(),
        newElement->FIFOIdx.entryIdx);
  }

  // Find the base element.
  for (auto baseS : this->baseStreams) {
    if (baseS->getLoopLevel() != this->getLoopLevel()) {
      continue;
    }

    auto &baseDynS = baseS->getLastDynamicStream();
    DYN_S_DPRINTF(baseDynS.dynamicStreamId, "BaseDynS.\n");
    if (baseS->stepRootStream == this->stepRootStream) {
      if (baseDynS.allocSize - baseDynS.stepSize <=
          dynS.allocSize - dynS.stepSize) {
        this->se->dumpFIFO();
        panic("Base %s has not enough allocated element for %s.",
              baseS->getStreamName().c_str(), this->getStreamName().c_str());
      }

      auto baseElement = baseDynS.stepped;
      auto element = dynS.stepped;
      while (element != nullptr) {
        if (!baseElement) {
          baseDynS.dump();
          S_PANIC(this, "Failed to find base element from %s.",
                  baseS->getStreamName());
        }
        element = element->next;
        baseElement = baseElement->next;
      }
      if (!baseElement) {
        S_PANIC(this, "Failed to find base element from %s.",
                baseS->getStreamName());
      }
      assert(baseElement != nullptr && "Failed to find base element.");
      newElement->baseElements.insert(baseElement);
    } else {
      // The other one must be a constant stream.
      assert(baseS->stepRootStream == nullptr &&
             "Should be a constant stream.");
      auto baseElement = baseDynS.stepped->next;
      assert(baseElement != nullptr && "Missing base element.");
      newElement->baseElements.insert(baseElement);
    }
  }

  auto newElementIdx = newElement->FIFOIdx.entryIdx;
  // Find the previous element of my self for reduction stream.
  if (this->isReduction()) {
    if (newElementIdx > 0) {
      assert(dynS.head != dynS.tail &&
             "Failed to find previous element for reduction stream.");
      S_ELEMENT_DPRINTF(newElement, "Found reduction dependence.");
      newElement->baseElements.insert(dynS.head);
    } else {
      // This is the first element. Let StreamElement::markAddrReady() set up
      // the initial value.
    }
  }
  if (newElementIdx > 0) {
    // Find the back base element, starting from the second element.
    for (auto backBaseS : this->backBaseStreams) {
      if (backBaseS->getLoopLevel() != this->getLoopLevel()) {
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
          newElement->baseElements.insert(baseElement);
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

  newElement->allocateCycle = cpuDelegator->curCycle();

  // Append to the list.
  dynS.head->next = newElement;
  dynS.head = newElement;
  dynS.allocSize++;
  this->allocSize++;

  if (newElement->FIFOIdx.entryIdx == 1) {
    if (newElement->FIFOIdx.streamId.coreId == 8 &&
        newElement->FIFOIdx.streamId.streamInstance == 1 &&
        newElement->FIFOIdx.streamId.staticId == 11704592) {
      S_ELEMENT_HACK(newElement, "Allocated.\n");
    }
  }

  S_ELEMENT_DPRINTF(newElement, "Allocated.\n");
}

StreamElement *Stream::releaseElementStepped() {

  /**
   * This function performs a normal release, i.e. release a stepped
   * element from the stream.
   */

  assert(!this->dynamicStreams.empty() && "No dynamic stream.");
  auto &dynS = this->dynamicStreams.front();

  assert(dynS.stepSize > 0 && "No element to release.");
  auto releaseElement = dynS.tail->next;
  assert(releaseElement->isStepped && "Release unstepped element.");

  const bool used = releaseElement->isFirstUserDispatched();
  bool late = false;

  this->statistic.numStepped++;
  if (used) {
    this->statistic.numUsed++;
    /**
     * Since this element is used by the core, we update the statistic
     * of the latency of this element experienced by the core.
     */
    if (releaseElement->valueReadyCycle < releaseElement->firstCheckCycle) {
      // The element is ready earlier than core's user.
      auto earlyCycles =
          releaseElement->firstCheckCycle - releaseElement->valueReadyCycle;
      this->statistic.numCoreEarlyElement++;
      this->statistic.numCoreEarlyCycle += earlyCycles;
    } else {
      // The element makes the core's user wait.
      auto lateCycles =
          releaseElement->valueReadyCycle - releaseElement->firstCheckCycle;
      this->statistic.numCoreLateElement++;
      this->statistic.numCoreLateCycle += lateCycles;
      late = true;
    }
  }
  dynS.updateReleaseCycle(this->cpuDelegator->curCycle(), late);

  // Update the aliased statistic.
  if (releaseElement->isAddrAliased) {
    this->statistic.numAliased++;
  }

  // Check if the element is faulted.
  if (this->isMemStream() && releaseElement->isAddrReady) {
    if (releaseElement->isValueFaulted(releaseElement->addr,
                                       releaseElement->size)) {
      this->statistic.numFaulted++;
    }
  }

  // Only do this for used elements.
  if (used) {
    this->handleConstUpdate(dynS, releaseElement);
    this->handleMergedPredicate(dynS, releaseElement);
  }

  dynS.tail->next = releaseElement->next;
  if (dynS.stepped == releaseElement) {
    dynS.stepped = dynS.tail;
  }
  if (dynS.head == releaseElement) {
    dynS.head = dynS.tail;
  }
  dynS.stepSize--;
  dynS.allocSize--;
  this->allocSize--;

  return releaseElement;
}

StreamElement *Stream::releaseElementUnstepped() {
  assert(!this->dynamicStreams.empty() && "No dynamic stream.");
  auto &dynS = this->dynamicStreams.front();
  if (dynS.allocSize == dynS.stepSize) {
    return nullptr;
  }
  // Check if the element is faulted.
  auto element = dynS.releaseElementUnstepped();
  S_ELEMENT_DPRINTF(element, "ReleaseElementUnstepped, isAddrReady %d.\n",
                    element->isAddrReady);
  if (this->isMemStream() && element->isAddrReady) {
    if (element->isValueFaulted(element->addr, element->size)) {
      this->statistic.numFaulted++;
    }
  }
  this->allocSize--;
  return element;
}

StreamElement *Stream::stepElement() {
  auto &dynS = this->getLastDynamicStream();
  auto element = dynS.stepped->next;
  assert(!element->isStepped && "Element already stepped.");
  element->isStepped = true;
  dynS.stepped = element;
  dynS.stepSize++;
  return element;
}

StreamElement *Stream::unstepElement() {
  auto &dynS = this->getLastDynamicStream();
  assert(dynS.stepSize > 0 && "No element to unstep.");
  auto element = dynS.stepped;
  assert(element->isStepped && "Element not stepped.");
  element->isStepped = false;
  // Search to get previous element.
  dynS.stepped = dynS.getPrevElement(element);
  dynS.stepSize--;
  return element;
}

StreamElement *Stream::getFirstUnsteppedElement() {
  auto &dynS = this->getLastDynamicStream();
  auto element = dynS.getFirstUnsteppedElement();
  if (!element) {
    this->se->dumpFIFO();
    S_PANIC(this,
            "No allocated element to use, TotalTripCount %d, EndDispatched %d.",
            dynS.totalTripCount, dynS.endDispatched);
  }
  return element;
}

StreamElement *Stream::getPrevElement(StreamElement *element) {
  auto &dynS = this->getDynamicStream(element->FIFOIdx.configSeqNum);
  return dynS.getPrevElement(element);
}

void Stream::handleConstUpdate(const DynamicStream &dynS,
                               StreamElement *element) {
  if (!(this->hasUpgradedToUpdate() && dynS.offloadedToCache)) {
    return;
  }
  this->performConstStore(dynS, element);
}

void Stream::handleMergedPredicate(const DynamicStream &dynS,
                                   StreamElement *element) {
  auto mergedPredicatedStreamIds = this->getMergedPredicatedStreams();
  if (!(mergedPredicatedStreamIds.size() > 0 && dynS.offloadedToCache)) {
    return;
  }
  // Prepare the predicate actual params.
  uint64_t elementVal = 0;
  element->getValue(element->addr, element->size,
                    reinterpret_cast<uint8_t *>(&elementVal));
  GetStreamValueFunc getStreamValue = [elementVal,
                                       element](uint64_t streamId) -> uint64_t {
    assert(streamId == element->FIFOIdx.streamId.staticId &&
           "Mismatch stream id for predication.");
    return elementVal;
  };
  auto params =
      convertFormalParamToParam(dynS.predFormalParams, getStreamValue);
  bool predTrue = dynS.predCallback->invoke(params) & 0x1;
  for (auto predStreamId : mergedPredicatedStreamIds) {
    S_ELEMENT_DPRINTF(element, "Predicate %d %d: %s.\n", predTrue,
                      predStreamId.pred_true(), predStreamId.id().name());
    if (predTrue != predStreamId.pred_true()) {
      continue;
    }
    auto predS = this->se->getStream(predStreamId.id().id());
    // They should be configured by the same configure instruction.
    const auto &predDynS = predS->getDynamicStream(dynS.configSeqNum);
    if (predS->getStreamType() == "store") {
      auto predElement = predDynS.getElementByIdx(element->FIFOIdx.entryIdx);
      if (!predElement) {
        S_ELEMENT_PANIC(element, "Failed to get predicated element.");
      }
      this->performConstStore(predDynS, predElement);
    } else {
      S_ELEMENT_PANIC(element, "Can only handle merged store stream.");
    }
  }
}

void Stream::performConstStore(const DynamicStream &dynS,
                               StreamElement *element) {
  /**
   * * Perform const store here.
   * We can not do that in the Ruby because it uses BackingStore, which
   * would cause the core to read the updated value.
   */
  auto elementVAddr = element->addr;
  auto elementSize = element->size;
  auto blockSize = cpuDelegator->cacheLineSize();
  auto elementLineOffset = elementVAddr % blockSize;
  assert(elementLineOffset + elementSize <= blockSize &&
         "Cannot support multi-line element with const update yet.");
  assert(elementSize <= sizeof(uint64_t) && "At most 8 byte element size.");
  Addr elementPAddr;
  assert(cpuDelegator->translateVAddrOracle(elementVAddr, elementPAddr) &&
         "Failed to translate address for const update.");
  // ! Hack: Just do a functional write.
  cpuDelegator->writeToMem(
      elementVAddr, elementSize,
      reinterpret_cast<const uint8_t *>(&dynS.constUpdateValue));
}

void Stream::dump() const {
  inform("Stream %50s =============================\n",
         this->getStreamName().c_str());
  for (const auto &dynS : this->dynamicStreams) {
    dynS.dump();
  }
}