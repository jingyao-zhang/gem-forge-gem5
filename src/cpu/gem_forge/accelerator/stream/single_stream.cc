#include "single_stream.hh"
#include "insts.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

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

SingleStream::SingleStream(const StreamArguments &args,
                           const LLVM::TDG::StreamInfo &_info)
    : Stream(args), info(_info) {

  const auto &relativeHistoryPath = this->info.history_path();
  auto historyPath =
      cpuDelegator->getTraceExtraFolder() + "/" + relativeHistoryPath;
  this->history =
      std::unique_ptr<StreamHistory>(new StreamHistory(historyPath));
  this->patternStream = std::unique_ptr<StreamPattern>(new StreamPattern(
      cpuDelegator->getTraceExtraFolder() + "/" + this->info.pattern_path()));

  for (const auto &baseStreamId : this->info.chosen_base_streams()) {
    auto baseStream = this->se->getStream(baseStreamId.id());
    this->addBaseStream(baseStream);
  }

  if (this->baseStreams.empty() && this->info.type() == "phi") {
    this->stepRootStream = this;
  }

  // Try to find the step root stream.
  for (auto &baseS : this->baseStreams) {
    if (baseS->getLoopLevel() != this->getLoopLevel()) {
      continue;
    }
    if (baseS->stepRootStream != nullptr) {
      if (this->stepRootStream != nullptr &&
          this->stepRootStream != baseS->stepRootStream) {
        panic("Double step root stream found.\n");
      }
      this->stepRootStream = baseS->stepRootStream;
    }
  }

  STREAM_DPRINTF("Initialized.\n");
}

SingleStream::~SingleStream() {}

bool SingleStream::isDirectLoadStream() const {
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

bool SingleStream::isPointerChaseLoadStream() const {
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
  // The base iv stream should have only one back dependence of myself.
  if (baseStream->backBaseStreams.size() != 1) {
    return false;
  }
  /**
   * `backStreamStreams` is `std::unordered_set<Stream *>`,
   * while `this` is `const Stream *`.
   * One way to solve this is to use is_transparent comparator
   * introduced in C++14. However, here I simply use a const
   * cast.
   */
  if (!baseStream->backBaseStreams.count(const_cast<SingleStream *>(this))) {
    return false;
  }
  return true;
}

void SingleStream::initializeBackBaseStreams() {
  for (const auto &backBaseStreamId : this->info.chosen_back_base_streams()) {
    assert(this->getStreamType() == "phi" &&
           "Only phi node can have back edge dependence.");
    auto backBaseStream = this->se->getStream(backBaseStreamId.id());
    this->addBackBaseStream(backBaseStream);
  }
}

const std::string &SingleStream::getStreamType() const {
  return this->info.type();
}

uint32_t SingleStream::getLoopLevel() const { return this->info.loop_level(); }

uint32_t SingleStream::getConfigLoopLevel() const {
  return this->info.config_loop_level();
}

int32_t SingleStream::getElementSize() const {
  return this->info.element_size();
}

void SingleStream::configure(uint64_t seqNum) {
  this->dispatchStreamConfig(seqNum);
  this->history->configure();
  this->patternStream->configure();
}

void SingleStream::prepareNewElement(StreamElement *element) {
  bool oracleUsed = false;
  auto nextValuePair = this->history->getNextAddr(oracleUsed);
  element->addr = nextValuePair.second;
  element->size = this->getElementSize();
}

uint64_t SingleStream::getTrueFootprint() const {
  return this->history->getNumCacheLines();
}

uint64_t SingleStream::getFootprint(unsigned cacheBlockSize) const { return 1; }

bool SingleStream::isContinuous() const { return false; }

void SingleStream::setupAddrGen(DynamicStream &dynStream,
                                const std::vector<uint64_t> *inputVec) {

  if (cpuDelegator->cpuType != GemForgeCPUDelegator::CPUTypeE::LLVM_TRACE) {
    // We have to use the pattern.
    assert(inputVec && "Missing InputVec when using execution simulation.");
    const auto &staticInfo = this->info.static_info();
    const auto &pattern = staticInfo.iv_pattern();
    assert(pattern.val_pattern() == ::LLVM::TDG::StreamValuePattern::LINEAR &&
           "So far only LINEAR pattern is supported for execution-driven "
           "simulation.");
    /**
     * We have two parameters for LINEAR pattern, base and stride.
     */
    assert(pattern.params_size() == 2 && "Missing parameters.");
    auto &formalParams = dynStream.formalParams;
    int inputIdx = 0;
    for (const auto &param : pattern.params()) {
      if (param.valid()) {
        // This is a valid parameter.
        hack("Find valid param #%d, val %llu", formalParams.size(),
             param.param());
        formalParams.emplace_back();
        auto &formalParam = formalParams.back();
        formalParam.isInvariant = true;
        formalParam.param.invariant = param.param();
      } else {
        // This should be a input.
        // TODO: Handle stream input for indirect streams.
        assert(inputIdx < inputVec->size() && "Overflow of inputVec.");
        hack("Find input param #%d, val %llu", formalParams.size(),
             inputVec->at(inputIdx));
        formalParams.emplace_back();
        auto &formalParam = formalParams.back();
        formalParam.isInvariant = true;
        formalParam.param.invariant = inputVec->at(inputIdx);
        inputIdx++;
      }
    }

    // Set the callback.
    dynStream.addrGenCallback =
        std::unique_ptr<LinearAddrGenCallback>(new LinearAddrGenCallback());
    return;
  }

  // So far just use the history callback.
  dynStream.addrGenCallback = this->history->allocateCallbackAtInstance(
      dynStream.dynamicStreamId.streamInstance);
  // No arguments needed for history information.
}

CacheStreamConfigureData *
SingleStream::allocateCacheConfigureData(uint64_t configSeqNum) {
  auto &dynStream = this->getDynamicStream(configSeqNum);
  auto history = std::make_shared<::LLVM::TDG::StreamHistory>(
      this->history->getHistoryAtInstance(
          dynStream.dynamicStreamId.streamInstance));
  return new CacheStreamConfigureData(this, dynStream.dynamicStreamId,
                                      this->getElementSize(), history);
}

uint64_t
SingleStream::getStreamLengthAtInstance(uint64_t streamInstance) const {
  return this->history->getHistoryAtInstance(streamInstance).history_size();
}

void SingleStream::dump() const {
  inform("Dump for stream %s.\n======================",
         this->getStreamName().c_str());
  inform("=========================\n");
}