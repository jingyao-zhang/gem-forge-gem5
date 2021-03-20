#include "stream.hh"
#include "insts.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"

// #include "base/misc.hh""
#include "base/trace.hh"
#include "proto/protoio.hh"

#include "debug/StreamBase.hh"
#include "debug/StreamCritical.hh"
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
    : staticId(args.staticId), streamName(args.name),
      dynInstance(DynamicStreamId::InvalidInstanceId), floatTracer(this),
      cpu(args.cpu), se(args.se) {

  this->allocSize = 0;
  this->stepSize = 0;
  this->maxSize = args.maxSize;
  this->stepRootStream = nullptr;
  this->lateFetchCount = 0;
  this->streamRegion = args.streamRegion;
}

Stream::~Stream() {
  for (auto &LS : this->logicals) {
    delete LS;
    LS = nullptr;
  }
  this->logicals.clear();
}

void Stream::dumpStreamStats(std::ostream &os) const {
  if (this->statistic.numConfigured == 0 &&
      this->statistic.numMisConfigured == 0) {
    // I have not been configured at all -- maybe only used in fast forward.
    return;
  }
  os << this->getStreamName() << '\n';
  this->statistic.dump(os);
  this->floatTracer.dump();
}

GemForgeCPUDelegator *Stream::getCPUDelegator() const {
  return this->se->getCPUDelegator();
}

bool Stream::isAtomicStream() const {
  return this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_AT;
}
bool Stream::isStoreStream() const {
  return this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_ST;
}
bool Stream::isLoadStream() const {
  return this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_LD;
}
bool Stream::isUpdateStream() const {
  return this->isLoadStream() && this->getEnabledStoreFunc();
}

bool Stream::isMemStream() const {
  switch (this->getStreamType()) {
  case ::LLVM::TDG::StreamInfo_Type_LD:
  case ::LLVM::TDG::StreamInfo_Type_ST:
  case ::LLVM::TDG::StreamInfo_Type_AT:
    return true;
  case ::LLVM::TDG::StreamInfo_Type_IV:
    return false;
  default:
    STREAM_PANIC("Invalid stream type.");
  }
}

void Stream::addAddrBaseStream(StaticId baseId, StaticId depId,
                               Stream *baseStream) {
  if (baseStream == this) {
    STREAM_PANIC("AddrBaseStream should not be self.");
  }
  this->addrBaseStreams.insert(baseStream);
  this->addrBaseEdges.emplace_back(depId, baseId, baseStream);

  baseStream->addrDepStreams.insert(this);
  baseStream->addrDepEdges.emplace_back(baseId, depId, this);
}

void Stream::addValueBaseStream(StaticId baseId, StaticId depId,
                                Stream *baseStream) {
  if (baseStream == this) {
    STREAM_PANIC("ValueBasetream should not be self.");
  }
  this->valueBaseStreams.insert(baseStream);
  this->valueBaseEdges.emplace_back(depId, baseId, baseStream);

  baseStream->valueDepStreams.insert(this);
  baseStream->valueDepEdges.emplace_back(baseId, depId, this);
}

void Stream::addBackBaseStream(StaticId baseId, StaticId depId,
                               Stream *backBaseStream) {
  this->backBaseStreams.insert(backBaseStream);
  this->backBaseEdges.emplace_back(depId, baseId, backBaseStream);
  backBaseStream->backDepStreams.insert(this);
  backBaseStream->backDepEdges.emplace_back(baseId, depId, this);
  if (this->isReduction()) {
    backBaseStream->hasBackDepReductionStream = true;
  }
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

void Stream::initializeAliasStreamsFromProtobuf(
    const ::LLVM::TDG::StaticStreamInfo &info) {
  auto aliasBaseStreamId = info.alias_base_stream().id();
  this->aliasBaseStream = this->se->tryGetStream(aliasBaseStreamId);
  /**
   * If can not find the alias base stream, simply use myself.
   */
  if (!this->aliasBaseStream) {
    this->aliasBaseStream = this;
  }
  this->aliasOffset = info.alias_offset();
  /**
   * Inform the AliasBaseStream that whether I am store.
   * Notice that this should not be implemented in the opposite way, as
   * AliasedStream may not be finalized yet, and getStreamType() may not be
   * available.
   */
  if (this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_ST) {
    this->aliasBaseStream->hasAliasedStoreStream = true;
  }
  /**
   * Same thing for update stream.
   */
  if (this->hasUpdate() && !this->getEnabledStoreFunc()) {
    // There are StoreStream that update this LoadStream.
    this->aliasBaseStream->hasAliasedStoreStream = true;
  }

  for (const auto &aliasedStreamId : info.aliased_streams()) {
    auto aliasedS = this->se->tryGetStream(aliasedStreamId.id());
    if (aliasedS) {
      // It's possible some streams may not be configured, e.g.
      // StoreStream that is merged into the LoadStream, making
      // them a UpdateStream.
      this->aliasedStreams.push_back(aliasedS);
    }
  }
}

void Stream::initializeCoalesceGroupStreams() {
  if (this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_IV) {
    // Not MemStream.
    return;
  }
  auto sid = this->getCoalesceBaseStreamId();
  if (sid == 0) {
    // This is invalid.
    return;
  }
  auto coalesceBaseS = this->se->getStream(sid);
  coalesceBaseS->coalesceGroupStreams.insert(this);
}

void Stream::dispatchStreamConfig(uint64_t seqNum, ThreadContext *tc) {
  // Allocate the new DynamicStream.
  this->dynamicStreams.emplace_back(this, this->allocateNewInstance(), seqNum,
                                    this->getCPUDelegator()->curCycle(), tc,
                                    this->se);
}

void Stream::executeStreamConfig(uint64_t seqNum,
                                 const DynamicStreamParamV *inputVec) {
  auto &dynStream = this->getDynamicStream(seqNum);
  assert(!dynStream.configExecuted && "StreamConfig already executed.");
  dynStream.configExecuted = true;

  /**
   * We intercept the constant update value here.
   */
  if (inputVec) {
    assert(!se->isTraceSim());
    DynamicStreamParamV inputVecCopy(*inputVec);
    this->extractExtraInputValues(dynStream, &inputVecCopy);
    this->setupAddrGen(dynStream, &inputVecCopy);
  } else {
    assert(se->isTraceSim());
    this->extractExtraInputValues(dynStream, nullptr);
    this->setupAddrGen(dynStream, nullptr);
  }

  /**
   * We are going to copy the total trip count from the step root stream.
   */
  if (this->stepRootStream) {
    const auto &rootDynS = this->stepRootStream->getDynamicStream(seqNum);
    assert(rootDynS.configExecuted &&
           "Root dynamic stream should be executed.");
    if (rootDynS.hasTotalTripCount()) {
      if (dynStream.hasTotalTripCount()) {
        assert(dynStream.getTotalTripCount() == rootDynS.getTotalTripCount() &&
               "Mismatch in TotalTripCount.");
      } else {
        dynStream.totalTripCount = rootDynS.getTotalTripCount();
        DYN_S_DPRINTF(dynStream.dynamicStreamId,
                      "Set TotalTripCount %llu from StepRoot.\n",
                      dynStream.totalTripCount);
      }
    } else {
      assert(!dynStream.hasTotalTripCount() &&
             "Missing TotalTripCount in StepRootStream.");
    }
  } else if (!this->backBaseStreams.empty()) {
    // Try to copy total trip count from back base stream.
    assert(this->backBaseStreams.size() == 1);
    auto backBaseS = *(this->backBaseStreams.begin());
    const auto &backBaseDynS = backBaseS->getDynamicStream(seqNum);
    if (backBaseDynS.configExecuted) {
      dynStream.totalTripCount = backBaseDynS.totalTripCount;
      DYN_S_DPRINTF(dynStream.dynamicStreamId,
                    "Set TotalTripCount %llu from BackBase.\n",
                    dynStream.totalTripCount);
    }
  }

  // Try to set total trip count for back dependent streams.
  for (auto backDepS : this->backDepStreams) {
    auto &backDepDynS = backDepS->getDynamicStream(seqNum);
    backDepDynS.totalTripCount = dynStream.totalTripCount;
    DYN_S_DPRINTF(backDepDynS.dynamicStreamId,
                  "Set TotalTripCount %llu from BackBase.\n",
                  backDepDynS.totalTripCount);
  }
}

void Stream::commitStreamConfig(uint64_t seqNum) {
  auto &dynStream = this->getDynamicStream(seqNum);
  assert(dynStream.configExecuted && "StreamConfig committed before executed.");
  assert(!dynStream.configCommitted && "StreamConfig already committed.");
  dynStream.configCommitted = true;
}

void Stream::rewindStreamConfig(uint64_t seqNum) {
  // Rewind must happen in reverse order.
  assert(!this->dynamicStreams.empty() &&
         "Missing DynamicStream when rewinding StreamConfig.");
  auto &dynStream = this->dynamicStreams.back();
  assert(dynStream.configSeqNum == seqNum && "Mismatch configSeqNum.");

  // Check if we have offloaded this.
  if (dynStream.offloadedToCache) {
    // Sink streams should have already been taken care of.
    panic("Don't support rewind StreamConfig for offloaded stream.");
  }

  /**
   * Get rid of any unstepped elements of the dynamic stream.
   */
  while (dynStream.allocSize > dynStream.stepSize) {
    this->se->releaseElementUnstepped(dynStream);
  }

  // Get rid of the dynamicStream.
  this->dynamicStreams.pop_back();

  assert(dynStream.allocSize == dynStream.stepSize &&
         "Unstepped elements when rewind StreamConfig.");
  this->statistic.numMisConfigured++;
}

bool Stream::isStreamConfigureExecuted(uint64_t seqNum) {
  auto &dynStream = this->getDynamicStream(seqNum);
  return dynStream.configExecuted;
}

void Stream::dispatchStreamEnd(uint64_t seqNum) {
  assert(this->isConfigured() && "Stream should be configured.");
  for (auto &dynS : this->dynamicStreams) {
    if (!dynS.endDispatched) {
      dynS.endDispatched = true;
      dynS.endSeqNum = seqNum;
      return;
    }
  }
  S_PANIC(this, "Failed to find ended DynStream.");
}

void Stream::rewindStreamEnd(uint64_t seqNum) {
  // Searching backwards to rewind StreamEnd.
  for (auto dynS = this->dynamicStreams.rbegin(),
            end = this->dynamicStreams.rend();
       dynS != end; ++dynS) {
    if (dynS->endDispatched && dynS->endSeqNum == seqNum) {
      dynS->endDispatched = false;
      dynS->endSeqNum = 0;
      return;
    }
  }
  S_PANIC(this, "Failed to find ended DynStream to rewind.");
}

void Stream::commitStreamEnd(uint64_t seqNum) {
  assert(!this->dynamicStreams.empty() &&
         "Empty dynamicStreams for StreamEnd.");
  auto &dynS = this->dynamicStreams.front();
  if (dynS.configSeqNum >= seqNum) {
    DYN_S_PANIC(dynS.dynamicStreamId,
                "Commit StreamEnd SeqNum %llu before ConfigSeqNum %llu.",
                seqNum, dynS.configSeqNum);
  }
  assert(dynS.configExecuted && "End before config executed.");
  assert(dynS.endDispatched && "End before end dispatched.");

  /**
   * We need to release all unstepped elements.
   */
  assert(dynS.stepSize == 0 && "Stepped but unreleased element.");
  assert(dynS.allocSize == 0 && "Unreleased element.");

  // Update stats of cycles.
  auto endCycle = this->getCPUDelegator()->curCycle();
  this->statistic.numCycle += endCycle - dynS.configCycle;

  // Update float stats.
  if (dynS.offloadedToCache) {
    this->statistic.numFloated++;
    if (dynS.pseudoOffloadedToCache) {
      this->statistic.numPseudoFloated++;
    }
  }

  // Remember the formal params history.
  this->recordAggregateHistory(dynS);

  this->dynamicStreams.pop_front();
}

void Stream::recordAggregateHistory(const DynamicStream &dynS) {
  if (this->aggregateHistory.size() == AggregateHistorySize) {
    this->aggregateHistory.pop_front();
  }
  this->aggregateHistory.emplace_back();
  auto &history = this->aggregateHistory.back();
  history.addrGenFormalParams = dynS.addrGenFormalParams;
  history.numReleasedElements = dynS.getNumReleasedElements();
  history.numIssuedRequests = dynS.getNumIssuedRequests();
  history.numPrivateCacheHits = dynS.getTotalHitPrivateCache();
  history.startVAddr = dynS.getStartVAddr();
}

DynamicStream &Stream::getDynamicStreamByInstance(InstanceId instance) {
  for (auto &dynStream : this->dynamicStreams) {
    if (dynStream.dynamicStreamId.streamInstance == instance) {
      return dynStream;
    }
  }
  S_PANIC(this, "Failed to find DynamicStream by Instance %llu.\n", instance);
}

DynamicStream &Stream::getDynamicStream(uint64_t seqNum) {
  for (auto &dynStream : this->dynamicStreams) {
    if (dynStream.configSeqNum == seqNum) {
      return dynStream;
    }
  }
  S_PANIC(this, "Failed to find DynamicStream by ConfigSeqNum %llu.\n", seqNum);
}

DynamicStream &Stream::getDynamicStreamByEndSeqNum(uint64_t seqNum) {
  for (auto &dynStream : this->dynamicStreams) {
    if (dynStream.endSeqNum == seqNum) {
      return dynStream;
    }
  }
  S_PANIC(this, "Failed to find DynamicStream by EndSeqNum %llu.\n", seqNum);
}

DynamicStream &Stream::getDynamicStreamBefore(uint64_t seqNum) {
  DynamicStream *dynS = nullptr;
  for (auto &dynStream : this->dynamicStreams) {
    if (dynStream.configSeqNum < seqNum) {
      dynS = &dynStream;
    }
  }
  if (!dynS) {
    S_PANIC(this, "Failed to find DynamicStream before SeqNum %llu.\n", seqNum);
  }
  return *dynS;
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
                                 const DynamicStreamParamV *inputVec,
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
      formalParam.invariant.uint64() = param.value();
    } else {
      // This should be an input.
      if (inputIdx >= inputVec->size()) {
        S_PANIC(this, "InputIdx (%d) overflowed, InputVec (%d).\n", inputIdx,
                inputVec->size());
      }
      // hack("Find input param #%d, val %llu.\n", formalParams.size(),
      //      inputVec->at(inputIdx));
      formalParam.invariant = inputVec->at(inputIdx);
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
  DYN_S_DPRINTF(dynStream.dynamicStreamId,
                "Setup LinearAddrGenCallback with Input params --------\n");
  for (auto param : *inputVec) {
    DYN_S_DPRINTF(dynStream.dynamicStreamId, "%llu\n", param.at(0));
  }
  DYN_S_DPRINTF(dynStream.dynamicStreamId,
                "Setup LinearAddrGenCallback with params --------\n");
  for (auto param : formalParams) {
    DYN_S_DPRINTF(dynStream.dynamicStreamId, "%s\n", param.invariant);
  }

  for (auto idx = 1; idx < formalParams.size() - 1; idx += 2) {
    auto &formalParam = formalParams.at(idx);
    // TripCount.
    auto tripCount = formalParam.invariant.uint64();
    // TotalTripCount.
    auto totalTripCount =
        (idx == 1) ? (tripCount)
                   : (tripCount * formalParams.at(idx - 2).invariant.uint64());
    formalParam.invariant.uint64() = totalTripCount;
  }

  DYN_S_DPRINTF(dynStream.dynamicStreamId,
                "Finalize LinearAddrGenCallback with params --------\n");
  for (auto param : formalParams) {
    DYN_S_DPRINTF(dynStream.dynamicStreamId, "%s\n", param.invariant);
  }

  // Set the callback.
  if (!this->addrGenCallback) {
    this->addrGenCallback = std::make_shared<LinearAddrGenCallback>();
  }
  dynStream.addrGenCallback = this->addrGenCallback;

  // Update the totalTripCount to the dynamic stream if possible.
  if (formalParams.size() % 2 == 1) {
    dynStream.totalTripCount =
        formalParams.at(formalParams.size() - 2).invariant.uint64();
  }
}

void Stream::setupFuncAddrFunc(DynamicStream &dynStream,
                               const DynamicStreamParamV *inputVec,
                               const LLVM::TDG::StreamInfo &info) {

  const auto &addrFuncInfo = info.addr_func_info();
  assert(addrFuncInfo.name() != "" && "Missing AddrFuncInfo.");
  auto &formalParams = dynStream.addrGenFormalParams;
  auto usedInputs =
      this->setupFormalParams(inputVec, addrFuncInfo, formalParams);
  assert(usedInputs == inputVec->size() && "Underflow of inputVec.");
  // Set the callback.
  if (!this->addrGenCallback) {
    auto addrExecFunc =
        std::make_shared<TheISA::ExecFunc>(dynStream.tc, addrFuncInfo);
    this->addrGenCallback = std::make_shared<FuncAddrGenCallback>(addrExecFunc);
  }
  dynStream.addrGenCallback = this->addrGenCallback;
}

int Stream::setupFormalParams(const DynamicStreamParamV *inputVec,
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
      formalParam.baseStreamId = arg.stream_id();
    } else {
      if (inputIdx >= inputVec->size()) {
        S_PANIC(this, "Missing input for %s: Given %llu, inputIdx %d.",
                info.name(), inputVec->size(), inputIdx);
      }
      // hack("Find invariant param #%d, val %llu.\n",
      // formalParams.size(),
      //      inputVec->at(inputIdx));
      formalParams.emplace_back();
      auto &formalParam = formalParams.back();
      formalParam.isInvariant = true;
      formalParam.invariant = inputVec->at(inputIdx);
      inputIdx++;
    }
  }
  return inputIdx;
}

/**
 * Extract extra input values from the inputVec. May modify inputVec.
 */
void Stream::extractExtraInputValues(DynamicStream &dynS,
                                     DynamicStreamParamV *inputVec) {

  /**
   * For trace simulation, we have no input.
   */
  if (se->isTraceSim()) {
    assert(this->getMergedPredicatedStreams().empty() &&
           "MergedPredicatedStreams in TraceSim.");
    assert(!this->getEnabledStoreFunc() && "StoreFunc in TraceSim.");
    assert(!this->getEnabledLoadFunc() && "LoadFunc in TraceSim.");
    assert(!this->isReduction() && "Reduction in TraceSim.");
    assert(!inputVec && "InputVec in TraceSim.");
    return;
  }

  /**
   * If this has is load stream with merged predicated stream, check for
   * any inputs for the predication function.
   */
  assert(inputVec && "Missing InputVec.");
  const auto &mergedPredicatedStreams = this->getMergedPredicatedStreams();
  if (mergedPredicatedStreams.size() > 0) {
    const auto &predFuncInfo = this->getPredicateFuncInfo();
    if (!this->predCallback) {
      this->predCallback =
          std::make_shared<TheISA::ExecFunc>(dynS.tc, predFuncInfo);
    }
    dynS.predCallback = this->predCallback;
    auto &predFormalParams = dynS.predFormalParams;
    auto usedInputs =
        this->setupFormalParams(inputVec, predFuncInfo, predFormalParams);
    // Consume these inputs.
    inputVec->erase(inputVec->begin(), inputVec->begin() + usedInputs);
  }
  /**
   * Handle StoreFunc.
   */
  if (this->getEnabledStoreFunc()) {
    assert(this->getStreamType() != ::LLVM::TDG::StreamInfo_Type_IV);
    const auto &storeFuncInfo = this->getStoreFuncInfo();
    if (!this->storeCallback) {
      this->storeCallback =
          std::make_shared<TheISA::ExecFunc>(dynS.tc, storeFuncInfo);
    }
    dynS.storeCallback = this->storeCallback;
    auto &storeFormalParams = dynS.storeFormalParams;
    auto usedInputs =
        this->setupFormalParams(inputVec, storeFuncInfo, storeFormalParams);
    // Consume these inputs.
    inputVec->erase(inputVec->begin(), inputVec->begin() + usedInputs);
  }
  /**
   * LoadFunc shares the same input as StoreFunc, for now.
   */
  if (this->getEnabledLoadFunc()) {
    const auto &info = this->getLoadFuncInfo();
    if (!this->loadCallback) {
      this->loadCallback = std::make_shared<TheISA::ExecFunc>(dynS.tc, info);
    }
    dynS.loadCallback = this->loadCallback;
    auto &loadFormalParams = dynS.loadFormalParams;
    if (this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_AT) {
      /**
       * For AtomicStreams, all inputs are the same as StoreFunc.
       */
      loadFormalParams = dynS.storeFormalParams;
    } else {
      auto usedInputs =
          this->setupFormalParams(inputVec, info, loadFormalParams);
      // Consume these inputs.
      inputVec->erase(inputVec->begin(), inputVec->begin() + usedInputs);
    }
  }
  /**
   * If this is a reduction stream, check for the initial value.
   */
  if (this->isReduction()) {
    assert(!inputVec->empty() && "Missing initial value for reduction stream.");
    dynS.initialValue = inputVec->front();
    inputVec->erase(inputVec->begin());
  }
}

CacheStreamConfigureDataPtr
Stream::allocateCacheConfigureData(uint64_t configSeqNum, bool isIndirect) {
  auto &dynStream = this->getDynamicStream(configSeqNum);
  auto configData = std::make_shared<CacheStreamConfigureData>(
      this, dynStream.dynamicStreamId, this->getMemElementSize(),
      dynStream.addrGenFormalParams, dynStream.addrGenCallback);

  // Set the totalTripCount.
  configData->totalTripCount = dynStream.totalTripCount;

  // Set the predication function.
  configData->predFormalParams = dynStream.predFormalParams;
  configData->predCallback = dynStream.predCallback;

  // Set the store and load func.
  configData->storeFormalParams = dynStream.storeFormalParams;
  configData->storeCallback = dynStream.storeCallback;
  configData->loadFormalParams = dynStream.loadFormalParams;
  configData->loadCallback = dynStream.loadCallback;

  // Set the reduction information.
  if (this->isReduction()) {
    assert(this->getMemElementSize() <= sizeof(StreamValue) &&
           "Cannot offload reduction stream larger than 64 bytes.");
  }
  configData->reductionInitValue = dynStream.initialValue;

  // Set the initial vaddr if this is not indirect stream.
  if (!isIndirect) {
    configData->initVAddr =
        dynStream.addrGenCallback
            ->genAddr(0, dynStream.addrGenFormalParams, getStreamValueFail)
            .front();
    // Remember to make line address.
    configData->initVAddr -=
        configData->initVAddr % this->getCPUDelegator()->cacheLineSize();

    Addr initPAddr;
    if (this->getCPUDelegator()->translateVAddrOracle(configData->initVAddr,
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

bool Stream::shouldComputeValue() const {
  if (!this->isMemStream()) {
    // IV/Reduction stream always compute value.
    return true;
  }
  if (this->isStoreComputeStream() || this->isLoadComputeStream() ||
      this->isUpdateStream()) {
    // StoreCompute/LoadCompute/Update stream can also compute.
    // AtomicCompute is handled by sending out the AMO request.
    return true;
  }
  return false;
}

bool Stream::isDirectMemStream() const {
  if (!this->isMemStream()) {
    return false;
  }
  // So far only only one base stream of phi type of the same loop level.
  for (auto baseS : this->addrBaseStreams) {
    if (baseS->getLoopLevel() != this->getLoopLevel()) {
      // Ignore streams from different loop level.
      continue;
    }
    if (baseS->isMemStream()) {
      // I depend on a MemStream and am indirect MemStream.
      return false;
    }
    if (!baseS->backBaseStreams.empty()) {
      return false;
    }
  }
  return true;
}

bool Stream::isDirectLoadStream() const {
  return this->isLoadStream() && this->isDirectMemStream();
}

bool Stream::isDirectStoreStream() const {
  return this->isStoreStream() && this->isDirectMemStream();
}

bool Stream::isIndirectLoadStream() const {
  return this->isLoadStream() && !this->isDirectMemStream();
}

bool Stream::isConfigured() const {
  if (this->dynamicStreams.empty() ||
      this->dynamicStreams.back().endDispatched) {
    return false;
  }
  return true;
}

DynamicStreamId Stream::allocateNewInstance() {
  this->dynInstance++;
  return DynamicStreamId(this->getCPUId(), this->staticId, this->dynInstance,
                         this->streamName.c_str());
}

void Stream::addValueBaseElements(StreamElement *newElement) {

  const auto &dynS = *newElement->dynS;
  auto newElementIdx = newElement->FIFOIdx.entryIdx;
  for (auto baseS : this->valueBaseStreams) {
    assert(baseS->getLoopLevel() == this->getLoopLevel());

    auto &baseDynS = baseS->getLastDynamicStream();
    DYN_S_DPRINTF(baseDynS.dynamicStreamId, "BaseDynS.\n");
    assert(baseS->stepRootStream == this->stepRootStream &&
           "ValueDep must have same StepRootStream.");
    if (baseDynS.allocSize - baseDynS.stepSize <=
        dynS.allocSize - dynS.stepSize) {
      this->se->dumpFIFO();
      S_PANIC(this, "ValueBase %s has not allocated element %llu.",
              baseS->getStreamName(), newElementIdx);
    }

    auto baseElement = baseDynS.getElementByIdx(newElementIdx);
    if (!baseElement) {
      S_PANIC(this, "Failed to find value base element from %s.",
              baseS->getStreamName());
    }
    newElement->valueBaseElements.emplace_back(baseElement);
  }

  /**
   * LoadComputeStream always has itself has the ValueBaseElement.
   */
  if (this->isLoadComputeStream()) {
    newElement->valueBaseElements.emplace_back(newElement);
  }

  if (newElementIdx == 0) {
    return;
  }

  auto prevElementIdx = newElementIdx - 1;
  for (auto baseS : this->backBaseStreams) {
    assert(baseS->getLoopLevel() == this->getLoopLevel());

    auto &baseDynS = baseS->getLastDynamicStream();
    DYN_S_DPRINTF(baseDynS.dynamicStreamId, "BackBaseDynS.\n");

    auto baseElement = baseDynS.getElementByIdx(prevElementIdx);
    if (!baseElement) {
      this->se->dumpFIFO();
      S_PANIC(this, "Failed to find back base element from %s.",
              baseS->getStreamName());
    }
    newElement->valueBaseElements.emplace_back(baseElement);
  }
}

StreamElement *Stream::releaseElementStepped(bool isEnd) {

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
    if (releaseElement->valueReadyCycle <
        releaseElement->firstValueCheckCycle) {
      // The element is ready earlier than core's user.
      auto earlyCycles = releaseElement->firstValueCheckCycle -
                         releaseElement->valueReadyCycle;
      this->statistic.numCoreEarlyElement++;
      this->statistic.numCoreEarlyCycle += earlyCycles;
    } else {
      // The element makes the core's user wait.
      auto lateCycles = releaseElement->valueReadyCycle -
                        releaseElement->firstValueCheckCycle;
      this->statistic.numCoreLateElement++;
      this->statistic.numCoreLateCycle += lateCycles;
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
  dynS.updateStatsOnReleaseStepElement(this->getCPUDelegator()->curCycle(),
                                       releaseElement->addr, late);

  // Update the aliased statistic.
  if (releaseElement->isAddrAliased) {
    this->statistic.numAliased++;
  }

  // Check if the element is faulted.
  if (this->isMemStream() && releaseElement->isAddrReady()) {
    if (releaseElement->isValueFaulted(releaseElement->addr,
                                       releaseElement->size)) {
      this->statistic.numFaulted++;
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

  S_ELEMENT_DPRINTF(releaseElement, "ReleaseElementStepped, used %d.\n", used);
  return releaseElement;
}

StreamElement *Stream::releaseElementUnstepped(DynamicStream &dynS) {
  auto element = dynS.releaseElementUnstepped();
  if (element) {
    this->allocSize--;
  }
  return element;
}

bool Stream::hasUnsteppedElement() {
  if (!this->isConfigured()) {
    // This must be wrong.
    return false;
  }
  auto &dynS = this->getFirstAliveDynStream();
  auto element = dynS.getFirstUnsteppedElement();
  if (!element) {
    // We don't have element for this used stream.
    DYN_S_DPRINTF(dynS.dynamicStreamId,
                  "NoUnsteppedElement config executed %d alloc %d stepped %d "
                  "total %d next %s.\n",
                  dynS.configExecuted, dynS.allocSize, dynS.stepSize,
                  dynS.totalTripCount, dynS.FIFOIdx);
    return false;
  }
  return true;
}

StreamElement *Stream::stepElement() {
  auto &dynS = this->getFirstAliveDynStream();
  auto element = dynS.getFirstUnsteppedElement();
  S_ELEMENT_DPRINTF(element, "Stepped.\n");
  element->isStepped = true;
  dynS.stepped = element;
  dynS.stepSize++;
  return element;
}

StreamElement *Stream::unstepElement() {
  auto &dynS = this->getFirstAliveDynStream();
  assert(dynS.stepSize > 0 && "No element to unstep.");
  auto element = dynS.stepped;
  assert(element->isStepped && "Element not stepped.");
  element->isStepped = false;
  // Search to get previous element.
  dynS.stepped = dynS.getPrevElement(element);
  dynS.stepSize--;
  return element;
}

DynamicStream &Stream::getFirstAliveDynStream() {
  for (auto &dynS : this->dynamicStreams) {
    if (!dynS.endDispatched) {
      return dynS;
    }
  }
  this->se->dumpFIFO();
  S_PANIC(this, "No Alive DynStream.");
}

DynamicStream *Stream::getAllocatingDynStream() {
  if (!this->isConfigured()) {
    return nullptr;
  }
  for (auto &dynS : this->dynamicStreams) {
    if (dynS.endDispatched) {
      continue;
    }
    // Check if we have reached the allocation limit for this one.
    if (dynS.hasTotalTripCount()) {
      if (dynS.FIFOIdx.entryIdx == dynS.getTotalTripCount() + 1) {
        // Notice the +1 for the Element consumed by StreamEnd.
        continue;
      }
    }
    return &dynS;
  }
  return nullptr;
}

StreamElement *Stream::getFirstUnsteppedElement() {
  auto &dynS = this->getFirstAliveDynStream();
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
  // Check if there is no previous element
  auto prevElement = dynS.getPrevElement(element);
  if (prevElement == dynS.tail) {
    // There is no valid previous element.
    return nullptr;
  }
  return prevElement;
}

std::unique_ptr<StreamAtomicOp>
Stream::setupAtomicOp(FIFOEntryIdx idx, int memElementsize,
                      const DynamicStreamFormalParamV &formalParams,
                      GetStreamValueFunc getStreamValue) {
  // Turn the FormalParams to ActualParams, except the last atomic
  // operand.
  assert(!formalParams.empty() && "AtomicOp has at least one operand.");
  DynamicStreamParamV params;
  for (int i = 0; i + 1 < formalParams.size(); ++i) {
    const auto &formalParam = formalParams.at(i);
    if (formalParam.isInvariant) {
      params.push_back(formalParam.invariant);
    } else {
      auto baseStreamId = formalParam.baseStreamId;
      auto baseStreamValue = getStreamValue(baseStreamId);
      params.push_back(baseStreamValue);
    }
  }
  // Push the final atomic operand as a dummy 0.
  const auto &formalAtomicParam = formalParams.back();
  assert(!formalAtomicParam.isInvariant && "AtomicOperand should be a stream.");
  assert(formalAtomicParam.baseStreamId == this->staticId &&
         "AtomicOperand should be myself.");
  params.emplace_back();
  auto atomicOp =
      m5::make_unique<StreamAtomicOp>(this, idx, memElementsize, params,
                                      this->storeCallback, this->loadCallback);
  return atomicOp;
}

void Stream::handleMergedPredicate(const DynamicStream &dynS,
                                   StreamElement *element) {
  auto mergedPredicatedStreamIds = this->getMergedPredicatedStreams();
  if (!(mergedPredicatedStreamIds.size() > 0 && dynS.offloadedToCache)) {
    return;
  }
  panic("Deprecated, need refactor.");
  // // Prepare the predicate actual params.
  // uint64_t elementVal = 0;
  // element->getValue(element->addr, element->size,
  //                   reinterpret_cast<uint8_t *>(&elementVal));
  // GetStreamValueFunc getStreamValue = [elementVal,
  //                                      element](uint64_t streamId) ->
  //                                      uint64_t {
  //   assert(streamId == element->FIFOIdx.streamId.staticId &&
  //          "Mismatch stream id for predication.");
  //   return elementVal;
  // };
  // auto params =
  //     convertFormalParamToParam(dynS.predFormalParams, getStreamValue);
  // bool predTrue = dynS.predCallback->invoke(params) & 0x1;
  // for (auto predStreamId : mergedPredicatedStreamIds) {
  //   S_ELEMENT_DPRINTF(element, "Predicate %d %d: %s.\n", predTrue,
  //                     predStreamId.pred_true(), predStreamId.id().name());
  //   if (predTrue != predStreamId.pred_true()) {
  //     continue;
  //   }
  //   auto predS = this->se->getStream(predStreamId.id().id());
  //   // They should be configured by the same configure instruction.
  //   const auto &predDynS = predS->getDynamicStream(dynS.configSeqNum);
  //   if (predS->getStreamType() == ::LLVM::TDG::StreamInfo_Type_ST) {
  //     auto predElement = predDynS.getElementByIdx(element->FIFOIdx.entryIdx);
  //     if (!predElement) {
  //       S_ELEMENT_PANIC(element, "Failed to get predicated element.");
  //     }
  //     this->performStore(predDynS, predElement, predDynS.constUpdateValue);
  //   } else {
  //     S_ELEMENT_PANIC(element, "Can only handle merged store stream.");
  //   }
  // }
}

void Stream::performStore(const DynamicStream &dynS, StreamElement *element,
                          uint64_t storeValue) {
  /**
   * * Perform const store here.
   * We can not do that in the Ruby because it uses BackingStore, which
   * would cause the core to read the updated value.
   */
  auto elementVAddr = element->addr;
  auto elementSize = element->size;
  auto blockSize = this->getCPUDelegator()->cacheLineSize();
  auto elementLineOffset = elementVAddr % blockSize;
  assert(elementLineOffset + elementSize <= blockSize &&
         "Cannot support multi-line element with const update yet.");
  assert(elementSize <= sizeof(uint64_t) && "At most 8 byte element size.");
  Addr elementPAddr;
  assert(this->getCPUDelegator()->translateVAddrOracle(elementVAddr,
                                                       elementPAddr) &&
         "Failed to translate address for const update.");
  // ! Hack: Just do a functional write.
  this->getCPUDelegator()->writeToMem(
      elementVAddr, elementSize,
      reinterpret_cast<const uint8_t *>(&storeValue));
}

void Stream::sampleStatistic() {
  this->statistic.numSample++;
  this->statistic.numInflyRequest += this->numInflyStreamRequests;
  this->statistic.maxSize += this->maxSize;
  this->statistic.allocSize += this->allocSize;
  this->statistic.numDynStreams += this->dynamicStreams.size();
}

void Stream::dump() const {
  inform("Stream %50s =============================\n",
         this->getStreamName().c_str());
  for (const auto &dynS : this->dynamicStreams) {
    dynS.dump();
  }
}