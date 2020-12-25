#include "coalesced_stream.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

// #include "base/misc.hh""
#include "base/trace.hh"
#include "proto/protoio.hh"

#include "debug/CoalescedStream.hh"
#define DEBUG_TYPE CoalescedStream
#include "stream_log.hh"

#include <sstream>

#define LOGICAL_STREAM_PANIC(S, format, args...)                               \
  panic("Logical Stream %s: " format, (S)->info.name().c_str(), ##args)

#define LS_DPRINTF(LS, format, args...)                                        \
  DPRINTF(CoalescedStream, "L-Stream %s: " format, (LS)->info.name().c_str(),  \
          ##args)

#define STREAM_DPRINTF(format, args...)                                        \
  DPRINTF(CoalescedStream, "C-Stream %s: " format,                             \
          this->getStreamName().c_str(), ##args)

LogicalStream::LogicalStream(const std::string &_traceExtraFolder,
                             const LLVM::TDG::StreamInfo &_info)
    : info(_info) {
  if (!this->info.history_path().empty()) {
    this->history = std::unique_ptr<StreamHistory>(
        new StreamHistory(_traceExtraFolder + "/" + this->info.history_path()));
    this->patternStream = std::unique_ptr<StreamPattern>(
        new StreamPattern(_traceExtraFolder + "/" + this->info.pattern_path()));
  }
}

LogicalStream::~LogicalStream() {}

CoalescedStream::CoalescedStream(const StreamArguments &args)
    : Stream(args), primeLStream(nullptr) {}

CoalescedStream::~CoalescedStream() {
  for (auto &LS : this->coalescedStreams) {
    delete LS;
    LS = nullptr;
  }
  this->coalescedStreams.clear();
}

void CoalescedStream::addStreamInfo(const LLVM::TDG::StreamInfo &info) {
  /**
   * Note: At this point the primary logical stream may not be created yet!
   */
  this->coalescedStreams.emplace_back(
      new LogicalStream(this->getCPUDelegator()->getTraceExtraFolder(), info));
}

void CoalescedStream::finalize() {
  this->selectPrimeLogicalStream();
  if (this->primeLStream->info.type() == ::LLVM::TDG::StreamInfo_Type_IV) {
    assert(this->getNumCoalescedStreams() == 1 && "Never coalesce IVStream");
  }
  // Initialize the dependence graph.
  this->initializeBaseStreams();
  this->initializeAliasStreams();
  this->initializeCoalesceGroupStreams();
  STREAM_DPRINTF("Finalized, ElementSize %d, LStreams: =========.\n",
                 this->coalescedElementSize);
  for (auto LS : this->coalescedStreams) {
    LS_DPRINTF(LS, "Offset %d, MemElementSize %d.\n", LS->getCoalesceOffset(),
               LS->getMemElementSize());
  }
  STREAM_DPRINTF("Finalized ====================================.\n");
}

void CoalescedStream::selectPrimeLogicalStream() {
  assert(!this->coalescedStreams.empty());
  // Other sanity check for coalesced streams.
  // Sort the streams with offset.
  std::sort(this->coalescedStreams.begin(), this->coalescedStreams.end(),
            [](const LogicalStream *LA, const LogicalStream *LB) -> bool {
              return LA->getCoalesceOffset() <= LB->getCoalesceOffset();
            });
  this->primeLStream = this->coalescedStreams.front();
  this->baseOffset = this->primeLStream->getCoalesceOffset();
  this->coalescedElementSize = this->primeLStream->getMemElementSize();
  assert(this->baseOffset >= 0 && "Illegal BaseOffset.");
  // Make sure we have the currect base_stream.
  for (const auto &LS : this->coalescedStreams) {
    assert(LS->getCoalesceBaseStreamId() ==
           this->primeLStream->getCoalesceBaseStreamId());
    // Compute the element size.
    this->coalescedElementSize = std::max(
        this->coalescedElementSize,
        LS->getCoalesceOffset() - this->baseOffset + LS->getMemElementSize());
  }

  // Sanity check for consistency between logical streams.
  for (const auto &LS : this->coalescedStreams) {
    const auto &LSStaticInfo = LS->info.static_info();
    const auto &PSStaticInfo = this->primeLStream->info.static_info();
#define CHECK_INFO(field)                                                      \
  do {                                                                         \
    auto A = LSStaticInfo.field();                                             \
    auto B = PSStaticInfo.field();                                             \
    if (A != B) {                                                              \
      panic("Mismatch in %s, %s, %s.", #field, LS->info.name(),                \
            primeLStream->info.name());                                        \
    }                                                                          \
  } while (false);
    CHECK_INFO(is_merged_predicated_stream);
    CHECK_INFO(no_core_user);
    CHECK_INFO(loop_level);
    CHECK_INFO(config_loop_level);
    CHECK_INFO(is_inner_most_loop);
    CHECK_INFO(compute_info().value_base_streams_size);
    CHECK_INFO(compute_info().enabled_store_func);
#undef CHECK_INFO
    // If more than one coalesced stream, then CoreElementSize must be
    // the same as the MemElementSize.
    if (this->coalescedStreams.size() > 1) {
      if (LS->getCoreElementSize() != LS->getMemElementSize()) {
        panic("Mismatch in %s CoreElementSize %d and MemElementSize %d.\n",
              LS->info.name(), LS->getCoreElementSize(),
              LS->getMemElementSize());
      }
    }
    for (const auto &sid : LS->getMergedLoadStoreBaseStreams()) {
      bool matched = false;
      for (const auto &tid :
           this->primeLStream->getMergedLoadStoreBaseStreams()) {
        if (tid.id() == sid.id()) {
          matched = true;
          break;
        }
      }
      assert(matched && "Failed to match MergedLoadStoreBaseStream.");
    }
  }
  /**
   * Finalize the stream name and static id.
   * ! This is important to get the correct StreamInput values.
   * ! I feel like some day I will pay the price due to this hacky
   * ! implementation.
   */
  this->streamName = this->primeLStream->info.name();
  this->staticId = this->primeLStream->info.id();
}

void CoalescedStream::initializeBaseStreams() {
  for (auto LS : this->coalescedStreams) {
    const auto &info = LS->info;
    const auto &loopLevel = info.static_info().loop_level();
    // Update the address dependence information.
    for (const auto &baseStreamId : info.chosen_base_streams()) {
      auto baseS = this->se->getStream(baseStreamId.id());
      assert(baseS != this && "Should never have circular address dependency.");
      this->addAddrBaseStream(baseStreamId.id(), info.id(), baseS);
    }

    // Update the value dependence information.
    for (const auto &baseId :
         info.static_info().compute_info().value_base_streams()) {
      auto baseS = this->se->getStream(baseId.id());
      assert(baseS != this && "Should never have circular value dependency.");
      this->addValueBaseStream(baseId.id(), info.id(), baseS);
    }

    // Update the back dependence information.
    for (const auto &baseId : info.chosen_back_base_streams()) {
      assert(this->getStreamType() == ::LLVM::TDG::StreamInfo_Type_IV &&
             "Only phi node can have back edge dependence.");
      if (this->coalescedStreams.size() != 1) {
        S_PANIC(this,
                "More than one logical stream has back edge dependence.\n");
      }
      auto baseS = this->se->getStream(baseId.id());
      this->addBackBaseStream(baseId.id(), info.id(), baseS);
    }

    // Reduction stream always has myself as the back base stream.
    if (this->isReduction()) {
      this->addBackBaseStream(info.id(), info.id(), this);
    }

    // Try to update the step root stream.
    for (auto &baseS : this->addrBaseStreams) {
      if (baseS->getLoopLevel() != loopLevel) {
        continue;
      }
      if (baseS->stepRootStream != nullptr) {
        if (this->stepRootStream != nullptr &&
            this->stepRootStream != baseS->stepRootStream) {
          S_PANIC(this,
                  "Double StepRootStream found in Base %s %u %u: Mine %s vs "
                  "New %s.\n",
                  baseS->getStreamName(), baseS->getLoopLevel(), loopLevel,
                  this->stepRootStream->getStreamName(),
                  baseS->stepRootStream->getStreamName());
        }
        this->stepRootStream = baseS->stepRootStream;
      }
    }
  }
  // Remember to set step root stream.
  // If there are no AddrBaseStreams, we take it as StepRoot.
  if (this->addrBaseStreams.empty()) {
    this->stepRootStream = this;
  }
}

void CoalescedStream::initializeAliasStreams() {
  // First sanity check for alias stream information.
  const auto &primeSSI = this->primeLStream->info.static_info();
  const auto &aliasBaseStreamId = primeSSI.alias_base_stream();
  for (auto &LS : this->coalescedStreams) {
    const auto &SSI = LS->info.static_info();
    if (SSI.alias_base_stream().id() != aliasBaseStreamId.id()) {
      S_PANIC(
          this,
          "Mismatch AliasBaseStream %llu (prime %llu) %llu (logical %llu).\n",
          aliasBaseStreamId.id(), this->primeLStream->getStreamId(),
          SSI.alias_base_stream().id(), LS->getStreamId());
    }
  }
  this->initializeAliasStreamsFromProtobuf(primeSSI);
}

void CoalescedStream::configure(uint64_t seqNum, ThreadContext *tc) {
  this->dispatchStreamConfig(seqNum, tc);
  if (se->isTraceSim()) {
    // We still use the trace based history address.
    for (auto &S : this->coalescedStreams) {
      S->history->configure();
      S->patternStream->configure();
    }
  }
}

::LLVM::TDG::StreamInfo_Type CoalescedStream::getStreamType() const {
  return this->primeLStream->info.type();
}

uint32_t CoalescedStream::getLoopLevel() const {
  /**
   * * finalize() will make sure that all logical streams ahve the same loop
   * * level.
   */
  return this->coalescedStreams.front()->info.static_info().loop_level();
}

uint32_t CoalescedStream::getConfigLoopLevel() const {
  return this->coalescedStreams.front()->info.static_info().config_loop_level();
}

bool CoalescedStream::isInnerMostLoop() const {
  return this->coalescedStreams.front()
      ->info.static_info()
      .is_inner_most_loop();
}

bool CoalescedStream::getFloatManual() const {
  return this->primeLStream->info.static_info().float_manual();
}

bool CoalescedStream::hasUpdate() const {
  return this->primeLStream->info.static_info()
             .compute_info()
             .update_stream()
             .id() != DynamicStreamId::InvalidStaticStreamId;
}

const Stream::PredicatedStreamIdList &
CoalescedStream::getMergedPredicatedStreams() const {
  return this->primeLStream->info.static_info().merged_predicated_streams();
}

const ::LLVM::TDG::ExecFuncInfo &CoalescedStream::getPredicateFuncInfo() const {
  return this->primeLStream->info.static_info().pred_func_info();
}

bool CoalescedStream::isMergedPredicated() const {
  return this->primeLStream->info.static_info().is_merged_predicated_stream();
}
bool CoalescedStream::isMergedLoadStoreDepStream() const {
  return this->primeLStream->info.static_info()
             .compute_info()
             .value_base_streams_size() > 0;
}
bool CoalescedStream::enabledStoreFunc() const {
  return this->primeLStream->info.static_info()
      .compute_info()
      .enabled_store_func();
}

const ::LLVM::TDG::StreamParam &CoalescedStream::getConstUpdateParam() const {
  assert(
      this->coalescedStreams.size() == 1 &&
      "Do not support constant update for more than 1 coalesced stream yet.");
  return this->primeLStream->info.static_info().const_update_param();
}

bool CoalescedStream::isReduction() const {
  if (this->primeLStream->info.static_info().val_pattern() ==
      ::LLVM::TDG::StreamValuePattern::REDUCTION) {
    assert(this->coalescedStreams.size() == 1 &&
           "CoalescedStream should never be reduction stream.");
    return true;
  }
  return false;
}

bool CoalescedStream::hasCoreUser() const {
  return !this->primeLStream->info.static_info().no_core_user();
}

bool CoalescedStream::isContinuous() const {
  const auto &pattern = this->primeLStream->patternStream->getPattern();
  if (pattern.val_pattern() != "LINEAR") {
    return false;
  }
  return this->getMemElementSize() == pattern.stride_i();
}

void CoalescedStream::setupAddrGen(DynamicStream &dynStream,
                                   const DynamicStreamParamV *inputVec) {

  if (se->isTraceSim()) {
    if (this->getNumCoalescedStreams() == 1) {
      // For simplicity.
      dynStream.addrGenCallback =
          this->primeLStream->history->allocateCallbackAtInstance(
              dynStream.dynamicStreamId.streamInstance);
    } else {
      S_PANIC(this, "Cannot setup addr gen for trace coalesced stream so far.");
    }
  } else {
    // We generate the address based on the primeLStream.
    assert(inputVec && "Missing InputVec.");
    const auto &info = this->primeLStream->info;
    const auto &staticInfo = info.static_info();
    const auto &pattern = staticInfo.iv_pattern();
    if (pattern.val_pattern() == ::LLVM::TDG::StreamValuePattern::LINEAR) {
      this->setupLinearAddrFunc(dynStream, inputVec, info);
      return;
    } else {
      // See there is an address function.
      const auto &addrFuncInfo = info.addr_func_info();
      if (addrFuncInfo.name() != "") {
        this->setupFuncAddrFunc(dynStream, inputVec, info);
        return;
      }
    }
  }
}

uint64_t
CoalescedStream::getStreamLengthAtInstance(uint64_t streamInstance) const {
  panic("Coalesced stream length at instance is not supported yet.\n");
}

uint64_t CoalescedStream::getFootprint(unsigned cacheBlockSize) const {
  /**
   * Estimate the memory footprint for this stream in number of unqiue cache
   * blocks. It is OK for us to under-estimate the footprint, as the cache will
   * try to cache a stream with low-memory footprint.
   */
  const auto &pattern = this->primeLStream->patternStream->getPattern();
  const auto totalElements =
      this->primeLStream->history->getCurrentStreamLength();
  if (pattern.val_pattern() == "LINEAR") {
    // One dimension linear stream.
    return totalElements * pattern.stride_i() / cacheBlockSize;
  } else if (pattern.val_pattern() == "QUARDRIC") {
    // For 2 dimention linear stream, first compute footprint of one row.
    auto rowFootprint =
        pattern.ni() * this->getMemElementSize() / cacheBlockSize;
    if (pattern.stride_i() > cacheBlockSize) {
      rowFootprint = pattern.ni();
    }
    /**
     * Now we check if there is any chance that the next row will overlap with
     * the previous row.
     */
    auto rowRange = std::abs(pattern.stride_i()) * pattern.ni();
    if (std::abs(pattern.stride_j()) < rowRange) {
      // There is a chance that the next row will overlap with the previous one.
      // Return one row footprint as an under-estimation.
      return rowFootprint;
    } else {
      // No chance of overlapping.
      return rowFootprint * (totalElements / pattern.ni());
    }
  } else {
    // For all other patterns, underestimate.
    return 1;
  }
}

uint64_t CoalescedStream::getTrueFootprint() const {
  return this->primeLStream->history->getNumCacheLines();
}

void CoalescedStream::getCoalescedOffsetAndSize(uint64_t streamId,
                                                int32_t &offset,
                                                int32_t &size) const {
  for (auto LS : this->coalescedStreams) {
    if (LS->getStreamId() == streamId) {
      offset = LS->getCoalesceOffset() - this->baseOffset;
      size = LS->getMemElementSize();
      return;
    }
  }
  S_PANIC(this, "Failed to find logical stream %llu.\n", streamId);
}
