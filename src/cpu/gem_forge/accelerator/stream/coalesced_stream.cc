#include "stream.hh"
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

void Stream::addStreamInfo(const LLVM::TDG::StreamInfo &info) {
  /**
   * Note: At this point the primary logical stream may not be created yet!
   */
  this->logicals.emplace_back(
      new LogicalStream(this->getCPUDelegator()->getTraceExtraFolder(), info));
}

void Stream::finalize() {
  this->selectPrimeLogicalStream();
  if (this->primeLogical->info.type() == ::LLVM::TDG::StreamInfo_Type_IV) {
    assert(this->getNumLogicalStreams() == 1 && "Never coalesce IVStream");
  }
  // Initialize the dependence graph.
  this->initializeBaseStreams();
  this->initializeAliasStreams();
  this->initializeCoalesceGroupStreams();
  STREAM_DPRINTF("Finalized, ElementSize %d, LStreams: =========.\n",
                 this->coalescedElementSize);
  for (auto LS : this->logicals) {
    LS_DPRINTF(LS, "Offset %d, MemElementSize %d.\n", LS->getCoalesceOffset(),
               LS->getMemElementSize());
  }
  STREAM_DPRINTF("Finalized ====================================.\n");
}

void Stream::selectPrimeLogicalStream() {
  assert(!this->logicals.empty());
  // Other sanity check for coalesced streams.
  // Sort the streams with offset.
  std::sort(this->logicals.begin(), this->logicals.end(),
            [](const LogicalStream *LA, const LogicalStream *LB) -> bool {
              return LA->getCoalesceOffset() <= LB->getCoalesceOffset();
            });
  this->primeLogical = this->logicals.front();
  this->baseOffset = this->primeLogical->getCoalesceOffset();
  this->coalescedElementSize = this->primeLogical->getMemElementSize();
  assert(this->baseOffset >= 0 && "Illegal BaseOffset.");
  // Make sure we have the currect base_stream.
  for (const auto &LS : this->logicals) {
    assert(LS->getCoalesceBaseStreamId() ==
           this->primeLogical->getCoalesceBaseStreamId());
    // Compute the element size.
    this->coalescedElementSize = std::max(
        this->coalescedElementSize,
        LS->getCoalesceOffset() - this->baseOffset + LS->getMemElementSize());
  }

  // Sanity check for consistency between logical streams.
  for (const auto &LS : this->logicals) {
    const auto &LSStaticInfo = LS->info.static_info();
    const auto &PSStaticInfo = this->primeLogical->info.static_info();
#define CHECK_INFO(field)                                                      \
  do {                                                                         \
    auto A = LSStaticInfo.field();                                             \
    auto B = PSStaticInfo.field();                                             \
    if (A != B) {                                                              \
      panic("Mismatch in %s, %s, %s.", #field, LS->info.name(),                \
            primeLogical->info.name());                                        \
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
    if (this->logicals.size() > 1) {
      if (LS->getCoreElementSize() != LS->getMemElementSize()) {
        panic("Mismatch in %s CoreElementSize %d and MemElementSize %d.\n",
              LS->info.name(), LS->getCoreElementSize(),
              LS->getMemElementSize());
      }
    }
    for (const auto &sid : LS->getMergedLoadStoreBaseStreams()) {
      bool matched = false;
      for (const auto &tid :
           this->primeLogical->getMergedLoadStoreBaseStreams()) {
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
  this->streamName = this->primeLogical->info.name();
  this->staticId = this->primeLogical->info.id();
}

void Stream::initializeBaseStreams() {
  for (auto LS : this->logicals) {
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
      if (this->logicals.size() != 1) {
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

void Stream::initializeAliasStreams() {
  // First sanity check for alias stream information.
  const auto &primeSSI = this->primeLogical->info.static_info();
  const auto &aliasBaseStreamId = primeSSI.alias_base_stream();
  for (auto &LS : this->logicals) {
    const auto &SSI = LS->info.static_info();
    if (SSI.alias_base_stream().id() != aliasBaseStreamId.id()) {
      S_PANIC(
          this,
          "Mismatch AliasBaseStream %llu (prime %llu) %llu (logical %llu).\n",
          aliasBaseStreamId.id(), this->primeLogical->getStreamId(),
          SSI.alias_base_stream().id(), LS->getStreamId());
    }
  }
  this->initializeAliasStreamsFromProtobuf(primeSSI);
}

/**
 * Only to configure all the history.
 */
void Stream::configure(uint64_t seqNum, ThreadContext *tc) {
  this->dispatchStreamConfig(seqNum, tc);
  if (se->isTraceSim()) {
    // We still use the trace based history address.
    for (auto &S : this->logicals) {
      S->history->configure();
      S->patternStream->configure();
    }
  }
}

bool Stream::isContinuous() const {
  const auto &pattern = this->primeLogical->patternStream->getPattern();
  if (pattern.val_pattern() != "LINEAR") {
    return false;
  }
  return this->getMemElementSize() == pattern.stride_i();
}

void Stream::setupAddrGen(DynamicStream &dynStream,
                          const DynamicStreamParamV *inputVec) {

  if (se->isTraceSim()) {
    if (this->getNumLogicalStreams() == 1) {
      // For simplicity.
      dynStream.addrGenCallback =
          this->primeLogical->history->allocateCallbackAtInstance(
              dynStream.dynamicStreamId.streamInstance);
    } else {
      S_PANIC(this, "Cannot setup addr gen for trace coalesced stream so far.");
    }
  } else {
    // We generate the address based on the primeLogical.
    assert(inputVec && "Missing InputVec.");
    const auto &info = this->primeLogical->info;
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

uint64_t Stream::getFootprint(unsigned cacheBlockSize) const {
  /**
   * Estimate the memory footprint for this stream in number of unqiue cache
   * blocks. It is OK for us to under-estimate the footprint, as the cache will
   * try to cache a stream with low-memory footprint.
   */
  const auto &pattern = this->primeLogical->patternStream->getPattern();
  const auto totalElements =
      this->primeLogical->history->getCurrentStreamLength();
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

uint64_t Stream::getTrueFootprint() const {
  return this->primeLogical->history->getNumCacheLines();
}

uint64_t Stream::getStreamLengthAtInstance(uint64_t streamInstance) const {
  panic("Coalesced stream length at instance is not supported yet.\n");
}

void Stream::getCoalescedOffsetAndSize(uint64_t streamId, int32_t &offset,
                                       int32_t &size) const {
  for (auto LS : this->logicals) {
    if (LS->getStreamId() == streamId) {
      offset = LS->getCoalesceOffset() - this->baseOffset;
      size = LS->getMemElementSize();
      return;
    }
  }
  S_PANIC(this, "Failed to find logical stream %llu.\n", streamId);
}
