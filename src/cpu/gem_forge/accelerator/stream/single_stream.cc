#include "single_stream.hh"
#include "insts.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "base/trace.hh"
#include "debug/StreamEngine.hh"
#include "proto/protoio.hh"

#include "debug/SingleStream.hh"
#define DEBUG_TYPE SingleStream
#include "stream_log.hh"

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

  S_DPRINTF(this, "Initialized.\n");
}

SingleStream::~SingleStream() {}

void SingleStream::finalize() {
  S_DPRINTF(this, "Finalized.\n");
  this->initializeBaseStreams();
  this->initializeBackBaseStreams();
}

void SingleStream::initializeBaseStreams() {
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
}

void SingleStream::initializeBackBaseStreams() {
  for (const auto &backBaseStreamId : this->info.chosen_back_base_streams()) {
    assert(this->getStreamType() == "phi" &&
           "Only phi node can have back edge dependence.");
    auto backBaseStream = this->se->getStream(backBaseStreamId.id());
    this->addBackBaseStream(backBaseStream);
  }
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

const std::string &SingleStream::getStreamType() const {
  return this->info.type();
}

int32_t SingleStream::getElementSize() const {
  return this->info.element_size();
}

void SingleStream::configure(uint64_t seqNum, ThreadContext *tc) {
  this->dispatchStreamConfig(seqNum, tc);
  // ! We are removing the hacky state machine inside the history.
  // this->history->configure();
  // this->patternStream->configure();
}

uint64_t SingleStream::getTrueFootprint() const {
  return this->history->getNumCacheLines();
}

uint64_t SingleStream::getFootprint(unsigned cacheBlockSize) const { return 1; }

bool SingleStream::isContinuous() const { return false; }

void SingleStream::setupAddrGen(DynamicStream &dynStream,
                                const std::vector<uint64_t> *inputVec) {

  S_DPRINTF(this, "Set up AddrGen for streamInstance %llu.\n",
            dynStream.dynamicStreamId.streamInstance);

  if (!se->isTraceSim()) {
    // We have to use the pattern.
    assert(inputVec && "Missing InputVec when using execution simulation.");
    const auto &staticInfo = this->info.static_info();
    const auto &pattern = staticInfo.iv_pattern();
    // First handle linear pattern.
    if (pattern.val_pattern() == ::LLVM::TDG::StreamValuePattern::LINEAR) {
      this->setupLinearAddrFunc(dynStream, inputVec, this->info);
      return;
    } else {
      // Check if there is an address function.
      const auto &addrFuncInfo = this->info.addr_func_info();
      if (addrFuncInfo.name() != "") {
        this->setupFuncAddrFunc(dynStream, inputVec, this->info);
        return;
      } else {
        S_PANIC(this, "Don't know how to generate the address.");
      }
    }
  }

  // So far just use the history callback.
  dynStream.addrGenCallback = this->history->allocateCallbackAtInstance(
      dynStream.dynamicStreamId.streamInstance);
  // No arguments needed for history information.
}

uint64_t
SingleStream::getStreamLengthAtInstance(uint64_t streamInstance) const {
  return this->history->getHistoryAtInstance(streamInstance).history_size();
}
