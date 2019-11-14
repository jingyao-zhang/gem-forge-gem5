#include "stream.hh"
#include "insts.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"

// #include "base/misc.hh""
#include "base/trace.hh"
#include "debug/StreamEngine.hh"
#include "proto/protoio.hh"

#define STREAM_DPRINTF(format, args...)                                        \
  DPRINTF(StreamEngine, "Stream %s: " format, this->getStreamName().c_str(),   \
          ##args)

#define STREAM_ENTRY_DPRINTF(entry, format, args...)                           \
  STREAM_DPRINTF("Entry (%lu, %lu): " format, (entry).idx.streamInstance,      \
                 (entry).idx.entryIdx, ##args)

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
      cpuDelegator(args.cpuDelegator), se(args.se), nilTail(args.se) {

  this->configured = false;
  this->head = &this->nilTail;
  this->stepped = &this->nilTail;
  this->tail = &this->nilTail;
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
  this->dynamicStreams.emplace_back(this->FIFOIdx.streamId, seqNum, tc,
                                    prevFIFOIdx);
}

void Stream::executeStreamConfig(uint64_t seqNum,
                                 const std::vector<uint64_t> *inputVec) {
  auto &dynStream = this->getDynamicStream(seqNum);
  assert(!dynStream.configExecuted && "StreamConfig already executed.");
  dynStream.configExecuted = true;
  this->setupAddrGen(dynStream, inputVec);
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

void Stream::commitStreamEnd(uint64_t seqNum) {
  assert(!this->dynamicStreams.empty() &&
         "Empty dynamicStreams for StreamEnd.");
  auto &endedDynamicStream = this->dynamicStreams.front();
  assert(endedDynamicStream.configSeqNum < seqNum && "End before config.");
  assert(endedDynamicStream.configExecuted && "End before config executed.");
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

void Stream::setupLinearAddrFunc(DynamicStream &dynStream,
                                 const std::vector<uint64_t> *inputVec,
                                 const LLVM::TDG::StreamInfo &info) {
  assert(inputVec && "Missing InputVec.");
  const auto &staticInfo = info.static_info();
  const auto &pattern = staticInfo.iv_pattern();
  assert(pattern.val_pattern() == ::LLVM::TDG::StreamValuePattern::LINEAR);
  /**
   * LINEAR pattern has 2n parameters, where n is the difference of loop
   * level between ConfigureLoop and InnerMostLoop.
   * It has the following format, starting from InnerMostLoop.
   * Stride0, [BackEdgeCount[i], Stride[i + 1]]*, Start
   * We will add 1 to BackEdgeCount to get the TripCount.
   */
  assert(pattern.params_size() % 2 == 0 &&
         "Number of parameters must be even.");
  assert(pattern.params_size() >= 2 && "Number of parameters must be >= 2.");
  auto &formalParams = dynStream.formalParams;
  auto inputIdx = 0;
  for (const auto &param : pattern.params()) {
    formalParams.emplace_back();
    auto &formalParam = formalParams.back();
    formalParam.isInvariant = true;
    if (param.valid()) {
      // This param comes from the Configuration.
      // hack("Find valid param #%d, val %llu.\n", formalParams.size(),
      //      param.param());
      formalParam.param.invariant = param.param();
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
}

CacheStreamConfigureData *
Stream::allocateCacheConfigureData(uint64_t configSeqNum) {
  auto &dynStream = this->getDynamicStream(configSeqNum);
  return new CacheStreamConfigureData(
      this, dynStream.dynamicStreamId, this->getElementSize(),
      dynStream.formalParams, dynStream.addrGenCallback);
}

bool Stream::isDirectLoadStream() const {
  if (this->getStreamType() != "load") {
    return false;
  }
  // So far only only one base stream of phi type.
  if (this->baseStreams.size() != 1) {
    return false;
  }
  auto baseStream = *(this->baseStreams.begin());
  if (baseStream->getStreamType() != "phi") {
    return false;
  }
  if (!baseStream->backBaseStreams.empty()) {
    return false;
  }
  return true;
}
