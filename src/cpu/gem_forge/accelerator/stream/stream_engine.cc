#include "stream_engine.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "base/trace.hh"
#include "debug/RubyStream.hh"
#include "debug/StreamEngine.hh"

namespace {
static std::string DEBUG_STREAM_NAME =
    "(IV acmod.c::1232(acmod_flags2list) bb19 bb19::tmp21(phi))";

bool isDebugStream(Stream *S) {
  return S->getStreamName() == DEBUG_STREAM_NAME;
}

void debugStream(Stream *S, const char *message) {
  inform("%20s: Stream %50s config %1d step %3d allocated %3d max %3d.\n",
         message, S->getStreamName().c_str(), S->configured, S->stepSize,
         S->allocSize, S->maxSize);
}

void debugStreamWithElements(Stream *S, const char *message) {
  inform("%20s: Stream %50s config %1d step %3d allocated %3d max %3d.\n",
         message, S->getStreamName().c_str(), S->configured, S->stepSize,
         S->allocSize, S->maxSize);
  std::stringstream ss;
  auto element = S->tail;
  while (element != S->head) {
    element = element->next;
    ss << element->FIFOIdx.entryIdx << '('
       << static_cast<int>(element->isAddrReady)
       << static_cast<int>(element->isValueReady) << ')';
    for (auto baseElement : element->baseElements) {
      ss << '.' << baseElement->FIFOIdx.entryIdx;
    }
    ss << ' ';
  }
  inform("%s\n", ss.str().c_str());
}
} // namespace

#define STREAM_DPRINTF(stream, format, args...)                                \
  DPRINTF(StreamEngine, "[%s]: " format, stream->getStreamName().c_str(),      \
          ##args)

#define STREAM_HACK(stream, format, args...)                                   \
  hack("[%s]: " format, stream->getStreamName().c_str(), ##args)

#define STREAM_ELEMENT_DPRINTF(element, format, args...)                       \
  STREAM_DPRINTF(element->getStream(), "[%lu, %lu]: " format,                  \
                 element->FIFOIdx.streamId.streamInstance,                     \
                 element->FIFOIdx.entryIdx, ##args)

#define STREAM_PANIC(stream, format, args...)                                  \
  panic("[%s]: " format, stream->getStreamName().c_str(), ##args)

#define STREAM_ELEMENT_PANIC(element, format, args...)                         \
  element->se->dump();                                                         \
  STREAM_PANIC(element->getStream(), "[%lu, %lu]: " format,                    \
               element->FIFOIdx.streamId.streamInstance,                       \
               element->FIFOIdx.entryIdx, ##args)

StreamEngine::StreamEngine()
    : TDGAccelerator(), streamPlacementManager(nullptr), isOracle(false),
      writebackCacheLine(nullptr), throttler(this), blockCycle(0),
      blockSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM) {}

StreamEngine::~StreamEngine() {
  if (this->streamPlacementManager != nullptr) {
    delete this->streamPlacementManager;
    this->streamPlacementManager = nullptr;
  }

  // Clear all the allocated streams.
  for (auto &streamIdStreamPair : this->streamMap) {
    /**
     * Be careful here as CoalescedStream are not newed, no need to delete them.
     */
    if (dynamic_cast<CoalescedStream *>(streamIdStreamPair.second) != nullptr) {
      continue;
    }

    delete streamIdStreamPair.second;
    streamIdStreamPair.second = nullptr;
  }
  this->streamMap.clear();
  delete[] this->writebackCacheLine;
  this->writebackCacheLine = nullptr;
}

void StreamEngine::handshake(LLVMTraceCPU *_cpu,
                             TDGAcceleratorManager *_manager) {
  TDGAccelerator::handshake(_cpu, _manager);

  auto cpuParams = dynamic_cast<const LLVMTraceCPUParams *>(_cpu->params());
  this->isOracle = cpuParams->streamEngineIsOracle;
  this->maxRunAHeadLength = cpuParams->streamEngineMaxRunAHeadLength;
  this->currentTotalRunAheadLength = 0;
  this->maxTotalRunAheadLength = cpuParams->streamEngineMaxTotalRunAHeadLength;
  // this->maxTotalRunAheadLength = this->maxRunAHeadLength * 512;
  if (cpuParams->streamEngineThrottling == "static") {
    this->throttlingStrategy = ThrottlingStrategyE::STATIC;
  } else if (cpuParams->streamEngineThrottling == "dynamic") {
    this->throttlingStrategy = ThrottlingStrategyE::DYNAMIC;
  } else {
    this->throttlingStrategy = ThrottlingStrategyE::GLOBAL;
  }
  this->enableLSQ = cpuParams->streamEngineEnableLSQ;
  this->enableCoalesce = cpuParams->streamEngineEnableCoalesce;
  this->enableMerge = cpuParams->streamEngineEnableMerge;
  this->enableStreamPlacement = cpuParams->streamEngineEnablePlacement;
  this->enableStreamPlacementOracle =
      cpuParams->streamEngineEnablePlacementOracle;
  this->enableStreamPlacementBus = cpuParams->streamEngineEnablePlacementBus;
  this->noBypassingStore = cpuParams->streamEngineNoBypassingStore;
  this->continuousStore = cpuParams->streamEngineContinuousStore;
  this->enablePlacementPeriodReset = cpuParams->streamEnginePeriodReset;
  this->placementLat = cpuParams->streamEnginePlacementLat;
  this->placement = cpuParams->streamEnginePlacement;
  this->writebackCacheLine = new uint8_t[cpu->system->cacheLineSize()];
  this->enableStreamFloat = cpuParams->streamEngineEnableFloat;
  this->enableStreamFloatIndirect = cpuParams->streamEngineEnableFloatIndirect;

  this->initializeFIFO(this->maxTotalRunAheadLength);

  if (this->enableStreamPlacement) {
    this->streamPlacementManager = new StreamPlacementManager(cpu, this);
  }
}

void StreamEngine::regStats() {

#define scalar(stat, describe)                                                 \
  this->stat.name(this->manager->name() + ("." #stat))                         \
      .desc(describe)                                                          \
      .prereq(this->stat)

  scalar(numConfigured, "Number of streams configured.");
  scalar(numStepped, "Number of streams stepped.");
  scalar(numElementsAllocated, "Number of stream elements allocated.");
  scalar(numElementsUsed, "Number of stream elements used.");
  scalar(numUnconfiguredStreamUse, "Number of unconfigured stream use.");
  scalar(numConfiguredStreamUse, "Number of configured stream use.");
  scalar(entryWaitCycles, "Number of cycles form first check to ready.");
  scalar(numStoreElementsAllocated,
         "Number of store stream elements allocated.");
  scalar(numStoreElementsStepped, "Number of store stream elements fetched.");
  scalar(numStoreElementsUsed, "Number of store stream elements used.");
  scalar(numLoadElementsAllocated, "Number of load stream elements allocated.");
  scalar(numLoadElementsFetched, "Number of load stream elements fetched.");
  scalar(numLoadElementsStepped, "Number of load stream elements fetched.");
  scalar(numLoadElementsUsed, "Number of load stream elements used.");
  scalar(numLoadElementWaitCycles,
         "Number of cycles from first check to ready for load element.");
  scalar(numLoadCacheLineUsed, "Number of cache line used.");
  scalar(numLoadCacheLineFetched, "Number of cache line fetched.");
  scalar(streamUserNotDispatchedByLoadQueue,
         "Number of cycles a stream user cannot dispatch due LQ full.");
  scalar(streamStoreNotDispatchedByStoreQueue,
         "Number of cycles a stream store cannot dispatch due SQ full.");
#undef scalar

  this->numTotalAliveElements.init(0, 1000, 50)
      .name(this->manager->name() + ".stream.numTotalAliveElements")
      .desc("Number of alive stream elements in each cycle.")
      .flags(Stats::pdf);
  this->numTotalAliveCacheBlocks.init(0, 1000, 50)
      .name(this->manager->name() + ".stream.numTotalAliveCacheBlocks")
      .desc("Number of alive cache blocks in each cycle.")
      .flags(Stats::pdf);
  this->numRunAHeadLengthDist.init(0, 15, 1)
      .name(this->manager->name() + ".stream.numRunAHeadLengthDist")
      .desc("Number of run ahead length for streams.")
      .flags(Stats::pdf);
  this->numTotalAliveMemStreams.init(0, 15, 1)
      .name(this->manager->name() + ".stream.numTotalAliveMemStreams")
      .desc("Number of alive memory stream.")
      .flags(Stats::pdf);

  this->numAccessPlacedInCacheLevel.init(3)
      .name(this->manager->name() + ".stream.numAccessPlacedInCacheLevel")
      .desc("Number of accesses placed in different cache level.")
      .flags(Stats::total);
  this->numAccessHitHigherThanPlacedCacheLevel.init(3)
      .name(this->manager->name() +
            ".stream.numAccessHitHigherThanPlacedCacheLevel")
      .desc("Number of accesses hit in higher level than placed cache.")
      .flags(Stats::total);
  this->numAccessHitLowerThanPlacedCacheLevel.init(3)
      .name(this->manager->name() +
            ".stream.numAccessHitLowerThanPlacedCacheLevel")
      .desc("Number of accesses hit in lower level than placed cache.")
      .flags(Stats::total);

  this->numAccessFootprintL1.init(0, 500, 100)
      .name(this->manager->name() + ".stream.numAccessFootprintL1")
      .desc("Number of accesses with footprint at L1.")
      .flags(Stats::pdf);
  this->numAccessFootprintL2.init(0, 4096, 1024)
      .name(this->manager->name() + ".stream.numAccessFootprintL2")
      .desc("Number of accesses with footprint at L2.")
      .flags(Stats::pdf);
  this->numAccessFootprintL3.init(0, 131072, 26214)
      .name(this->manager->name() + ".stream.numAccessFootprintL3")
      .desc("Number of accesses with footprint at L3.")
      .flags(Stats::pdf);
}

bool StreamEngine::canStreamConfig(const StreamConfigInst *inst) const {
  /**
   * A stream can be configured iff. we can guarantee that it will be allocate
   * one entry when configured.
   *
   * If this this the first time we encounter the stream, we check the number of
   * free entries. Otherwise, we ALSO ensure that allocSize < maxSize.
   */

  // hack("Configure for loop %s.\n",
  //      inst->getTDG().stream_config().loop().c_str());
  /**
   * ! I need to enforce a certain dependence.
   */
  if (blockCycle > 0) {
    return false;
  }
  if (inst->getTDG().stream_config().loop() ==
          "linear.c::844(solve_l2r_l1l2_svc)::bb218" ||
      inst->getTDG().stream_config().loop() ==
          "linear.c::844(solve_l2r_l1l2_svc)::bb295") {
    // Jesus adhoc fix.
    // if (blockSeqNum < inst->getSeqNum()) {
    //   // A new blocking one.
    //   blockSeqNum = inst->getSeqNum();
    //   blockCycle = 1000;
    //   return false;
    // }
  }
  auto infoRelativePath = inst->getTDG().stream_config().info_path();
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);
  auto configuredStreams = this->enableCoalesce
                               ? streamRegion.coalesced_stream_ids_size()
                               : streamRegion.streams_size();

  // Sanity check on the number of configured streams.
  {
    if (configuredStreams * 3 > this->maxTotalRunAheadLength) {
      panic("Too many streams configuredStreams for loop %s %d, FIFOSize %d.\n",
            inst->getTDG().stream_config().loop().c_str(), configuredStreams,
            this->maxTotalRunAheadLength);
    }
  }

  if (this->numFreeFIFOEntries < configuredStreams) {
    // Not enough free entries for each stream.
    return false;
  }

  // Check that allocSize < maxSize.
  if (this->enableCoalesce) {
    for (const auto &streamId : streamRegion.coalesced_stream_ids()) {
      auto iter = this->streamMap.find(streamId);
      if (iter != this->streamMap.end()) {
        // Check if we have quota for this stream.
        auto S = iter->second;
        if (S->allocSize == S->maxSize) {
          // No more quota.
          return false;
        }
      }
    }
  } else {
    for (const auto &streamInfo : streamRegion.streams()) {
      auto streamId = streamInfo.id();
      auto iter = this->streamMap.find(streamId);
      if (iter != this->streamMap.end()) {
        // Check if we have quota for this stream.
        auto S = iter->second;
        if (S->allocSize == S->maxSize) {
          // No more quota.
          return false;
        }
      }
    }
  }
  return true;
}

void StreamEngine::dispatchStreamConfigure(StreamConfigInst *inst) {
  assert(this->canStreamConfig(inst) && "Cannot configure stream.");

  this->numConfigured++;

  auto infoRelativePath = inst->getTDG().stream_config().info_path();
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);

  // Initialize all the streams if this is the first time we encounter the loop.
  for (const auto &streamInfo : streamRegion.streams()) {
    const auto &streamId = streamInfo.id();
    // Remember to also check the coalesced id map.
    if (this->streamMap.count(streamId) == 0 &&
        this->coalescedStreamIdMap.count(streamId) == 0) {
      // We haven't initialize streams in this loop.
      hack("Initialize due to stream %lu.\n", streamId);
      this->initializeStreams(streamRegion);
      break;
    }
  }

  /**
   * Get all the configured streams.
   */
  std::list<Stream *> configStreams;
  std::unordered_set<Stream *> dedupSet;
  for (const auto &streamInfo : streamRegion.streams()) {
    // Deduplicate the streams due to coalescing.
    const auto &streamId = streamInfo.id();
    auto stream = this->getStream(streamId);
    if (dedupSet.count(stream) == 0) {
      configStreams.push_back(stream);
      dedupSet.insert(stream);
    }
  }

  for (auto &S : configStreams) {
    assert(!S->configured && "The stream should not be configured.");
    S->configured = true;
    S->statistic.numConfigured++;

    /**
     * 1. Clear all elements between stepHead and allocHead.
     * 2. Create the new index.
     * 3. Allocate more entries.
     */

    // 1. Release elements.
    while (S->allocSize > S->stepSize) {
      assert(S->stepped->next != nullptr && "Missing next element.");
      auto releaseElement = S->stepped->next;
      S->stepped->next = releaseElement->next;
      S->allocSize--;
      if (S->head == releaseElement) {
        S->head = S->stepped;
      }
      this->addFreeElement(releaseElement);
    }

    // Only to configure the history for single stream.
    S->configure(inst);

    // 2. Create new index.
    S->FIFOIdx.newInstance(inst->getSeqNum());

    /**
     * Initialize the DynamicInstanceState.
     */
    S->dynamicInstanceStates.emplace_back(S->FIFOIdx.streamId,
                                          inst->getSeqNum());
  }

  // 3. Allocate new entries one by one for all streams.
  // The first element is guaranteed to be allocated.
  for (auto S : configStreams) {
    // hack("Allocate element for stream %s.\n", S->getStreamName().c_str());
    assert(this->hasFreeElement());
    assert(S->allocSize < S->maxSize);
    assert(this->areBaseElementAllocated(S));
    this->allocateElement(S);
  }
  for (auto S : configStreams) {
    if (isDebugStream(S)) {
      debugStream(S, "Dispatch Config");
      if (S->allocSize < 1) {
        panic("Failed to allocate one number of elements.");
      }
    }
  }
}

void StreamEngine::executeStreamConfigure(StreamConfigInst *inst) {

  auto infoRelativePath = inst->getTDG().stream_config().info_path();
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);

  /**
   * Get all the configured streams.
   */
  std::list<Stream *> configStreams;
  std::unordered_set<Stream *> dedupSet;
  for (const auto &streamInfo : streamRegion.streams()) {
    // Deduplicate the streams due to coalescing.
    const auto &streamId = streamInfo.id();
    auto stream = this->getStream(streamId);
    if (dedupSet.count(stream) == 0) {
      configStreams.push_back(stream);
      dedupSet.insert(stream);
    }
  }

  for (auto &S : configStreams) {
    // Simply notify the stream.
    S->executeStreamConfigure(inst);
    /**
     * StreamAwareCache: Send a StreamConfigReq to the cache hierarchy.
     */
    if (this->enableStreamFloat) {
      // Try to find the DynamicInstanceState.
      Stream::DynamicInstanceState *dynamicInstanceState = nullptr;
      for (auto &state : S->dynamicInstanceStates) {
        if (state.configSeqNum == inst->getSeqNum()) {
          // We found the dynamicInstanceState.
          dynamicInstanceState = &state;
          break;
        }
      }
      assert(dynamicInstanceState != nullptr &&
             "Failed to find the DynamicInstanceState.");

      if (this->shouldOffloadStream(
              S, dynamicInstanceState->dynamicStreamId.streamInstance)) {

        // Remember the offloaded decision.
        // ! Only do this for the root offloaded stream.
        dynamicInstanceState->offloadedToCache = true;

        // Get the CacheStreamConfigureData.
        auto streamConfigureData =
            S->allocateCacheConfigureData(inst->getSeqNum());

        // Set up the init physical address.
        if (cpu->isStandalone()) {
          auto initPAddr =
              cpu->translateAndAllocatePhysMem(streamConfigureData->initVAddr);
          streamConfigureData->initPAddr = initPAddr;
        } else {
          panic("Stream so far can only work in standalone mode.");
        }

        if (S->isPointerChaseLoadStream()) {
          streamConfigureData->isPointerChase = true;
        }

        /**
         * If we enable indirect stream to float, so far we add only
         * one indirect stream here.
         */
        if (this->enableStreamFloatIndirect) {
          for (auto dependentStream : S->dependentStreams) {
            if (dependentStream->getStreamType() == "load") {
              if (dependentStream->baseStreams.size() == 1) {
                // Only dependent on this direct stream.
                streamConfigureData->indirectStreamConfigure =
                    std::shared_ptr<CacheStreamConfigureData>(
                        dependentStream->allocateCacheConfigureData(
                            inst->getSeqNum()));
                break;
              }
            }
          }
          if (streamConfigureData->indirectStreamConfigure == nullptr) {
            // Not found a valid indirect stream, let's try to search for
            // a indirect stream that is one iteration behind.
            for (auto backDependentStream : S->backDependentStreams) {
              if (backDependentStream->getStreamType() != "phi") {
                continue;
              }
              if (backDependentStream->backBaseStreams.size() != 1) {
                continue;
              }
              for (auto indirectStream :
                   backDependentStream->dependentStreams) {
                if (indirectStream == S) {
                  continue;
                }
                if (indirectStream->getStreamType() != "load") {
                  continue;
                }
                if (indirectStream->baseStreams.size() != 1) {
                  continue;
                }
                // We found one valid indirect stream that is one iteration
                // behind S.
                streamConfigureData->indirectStreamConfigure =
                    std::shared_ptr<CacheStreamConfigureData>(
                        indirectStream->allocateCacheConfigureData(
                            inst->getSeqNum()));
                streamConfigureData->indirectStreamConfigure
                    ->isOneIterationBehind = true;
                break;
              }
              if (streamConfigureData->indirectStreamConfigure != nullptr) {
                // We already found one.
                break;
              }
            }
          }
        }

        auto pkt = TDGPacketHandler::createStreamControlPacket(
            streamConfigureData->initPAddr, cpu->getDataMasterID(), 0,
            MemCmd::Command::StreamConfigReq,
            reinterpret_cast<uint64_t>(streamConfigureData));
        DPRINTF(RubyStream,
                "Create StreamConfig pkt %#x %#x, initVAddr: %#x, initPAddr "
                "%#x.\n",
                pkt, streamConfigureData, streamConfigureData->initVAddr,
                streamConfigureData->initPAddr);
        cpu->sendRequest(pkt);
      }
    }
  }
}

void StreamEngine::commitStreamConfigure(StreamConfigInst *inst) {
  // So far we don't need to do anything.
}

bool StreamEngine::canStreamStep(const StreamStepInst *inst) const {
  /**
   * For all the streams get stepped, make sure that
   * allocSize - stepSize >= 2.
   */
  auto stepStreamId = inst->getTDG().stream_step().stream_id();
  auto stepStream = this->getStream(stepStreamId);

  bool canStep = true;
  for (auto S : this->getStepStreamList(stepStream)) {
    if (S->allocSize - S->stepSize < 2) {
      canStep = false;
      break;
    }
  }
  // hack("Check if can step stream %s: %d.\n",
  //      stepStream->getStreamName().c_str(), canStep);
  return canStep;
}

void StreamEngine::dispatchStreamStep(StreamStepInst *inst) {
  /**
   * For all the streams get stepped, increase the stepped pointer.
   */

  assert(this->canStreamStep(inst) && "canStreamStep assertion failed.");
  this->numStepped++;

  auto stepStreamId = inst->getTDG().stream_step().stream_id();
  auto stepStream = this->getStream(stepStreamId);

  // hack("Step stream %s.\n", stepStream->getStreamName().c_str());

  for (auto S : this->getStepStreamList(stepStream)) {
    assert(S->configured && "Stream should be configured to be stepped.");
    S->stepped = S->stepped->next;
    S->stepSize++;
  }
  /**
   * * Enforce that stepSize is the same within the stepGroup.
   * ! This may be the case if they are configured in different loop level.
   * TODO Fix this corner case.
   */
  // for (auto S : this->getStepStreamList(stepStream)) {
  //   if (S->stepSize != stepStream->stepSize) {
  //     this->dumpFIFO();
  //     panic("Streams within the same stepGroup should have the same
  //     stepSize:
  //     "
  //           "%s, %s.",
  //           S->getStreamName().c_str(),
  //           stepStream->getStreamName().c_str());
  //   }
  // }
  if (isDebugStream(stepStream)) {
  }
}

void StreamEngine::commitStreamStep(StreamStepInst *inst) {
  auto stepStreamId = inst->getTDG().stream_step().stream_id();
  auto stepStream = this->getStream(stepStreamId);

  const auto &stepStreams = this->getStepStreamList(stepStream);

  for (auto S : stepStreams) {
    /**
     * 1. Why only throttle for streamStep?
     * Normally you want to throttling when you release the element.
     * However, so far the throttling is constrainted by the
     * totalRunAheadLength, which only considers configured streams.
     * Therefore, we can not throttle for the last element (streamEnd), as
     * some base streams may already be cleared, and we get an inaccurate
     * totalRunAheadLength, causing the throttling to exceed the limit and
     * deadlock.
     *
     * To solve this, we only do throttling for streamStep.
     *
     * 2. How to handle short streams?
     * There is a pathological case when the streams are short, and increasing
     * the run ahead length beyond the stream length does not make sense.
     * We do not throttle if the element is within the run ahead length.
     */
    auto releaseElement = S->tail->next;
    assert(releaseElement->FIFOIdx.configSeqNum !=
               LLVMDynamicInst::INVALID_SEQ_NUM &&
           "This element does not have valid config sequence number.");
    if (releaseElement->FIFOIdx.entryIdx > S->maxSize) {
      this->throttleStream(S, releaseElement);
    }
    this->releaseElement(S);
  }

  // ! Do not allocate here.
  // ! allocateElements() will handle it.

  if (isDebugStream(stepStream)) {
  }
}

int StreamEngine::getStreamUserLQEntries(const LLVMDynamicInst *inst) const {
  // Only care this if we enable lsq for the stream engine.
  if (!this->enableLSQ) {
    return 0;
  }
  // Collect all the element used.
  std::unordered_set<StreamElement *> usedElementSet;
  for (const auto &dep : inst->getTDG().deps()) {
    if (dep.type() != ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      continue;
    }
    auto streamId = dep.dependent_id();
    auto S = this->getStream(streamId);
    if (!S->configured) {
      // Ignore the out-of-loop use (see dispatchStreamUser).
      continue;
    }
    if (S->allocSize <= S->stepSize) {
      inst->dumpBasic();
      this->dumpFIFO();
      panic("No allocated element to use for stream %s.",
            S->getStreamName().c_str());
    }
    usedElementSet.insert(S->stepped->next);
  }
  /**
   * The only thing we need to worry about is to check there are
   * enough space in the load queue to hold all the first use of the
   * load stream element.
   */
  auto firstUsedLoadStreamElement = 0;
  for (auto &element : usedElementSet) {
    if (element->stream->getStreamType() != "load") {
      // Not a load stream. Ignore it.
      continue;
    }
    if (element->firstUserSeqNum != LLVMDynamicInst::INVALID_SEQ_NUM) {
      // Not the first user of the load stream element. Ignore it.
      continue;
    }
    firstUsedLoadStreamElement++;
  }

  return firstUsedLoadStreamElement;
}

std::list<std::unique_ptr<GemForgeLQCallback>>
StreamEngine::createStreamUserLQCallbacks(LLVMDynamicInst *inst) {
  std::list<std::unique_ptr<GemForgeLQCallback>> callbacks;
  auto &elementSet = this->userElementMap.at(inst);
  for (auto &element : elementSet) {
    if (element == nullptr) {
      continue;
    }
    if (element->stream->getStreamType() != "load") {
      // Not a load stream.
      continue;
    }
    if (element->firstUserSeqNum == inst->getSeqNum()) {
      // Insert into the load queue if we model the lsq.
      if (this->enableLSQ) {
        callbacks.emplace_back(
            new GemForgeStreamEngineLQCallback(element, inst, this->cpu));
      }
    }
  }
  return callbacks;
}

void StreamEngine::dispatchStreamUser(LLVMDynamicInst *inst) {
  assert(this->userElementMap.count(inst) == 0);

  auto &elementSet =
      this->userElementMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(inst),
                   std::forward_as_tuple())
          .first->second;

  for (const auto &dep : inst->getTDG().deps()) {
    if (dep.type() != ::LLVM::TDG::TDGInstructionDependence::STREAM) {
      continue;
    }
    auto streamId = dep.dependent_id();
    auto S = this->getStream(streamId);

    /**
     * It is possible that the stream is unconfigured (out-loop use).
     * In such case we assume it's ready and use a nullptr as a special
     * element
     */
    if (!S->configured) {
      elementSet.insert(nullptr);
    } else {
      if (S->allocSize <= S->stepSize) {
        inst->dumpBasic();
        this->dumpFIFO();
        panic("No allocated element to use for stream %s.",
              S->getStreamName().c_str());
      }

      auto element = S->stepped->next;
      // Mark the first user sequence number.
      if (element->firstUserSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
        element->firstUserSeqNum = inst->getSeqNum();
      }
      elementSet.insert(element);
      // Construct the elementUserMap.
      this->elementUserMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(element),
                   std::forward_as_tuple())
          .first->second.insert(inst);
    }
  }
}

bool StreamEngine::areUsedStreamsReady(const LLVMDynamicInst *inst) {
  assert(this->userElementMap.count(inst) != 0);

  bool ready = true;
  for (auto &element : this->userElementMap.at(inst)) {
    if (element == nullptr) {
      /**
       * Sometimes there is use after stream end,
       * in such case we assume the element is copied to register and
       * is ready.
       */
      continue;
    }
    // Mark the first check cycle.
    if (element->firstCheckCycle == 0) {
      element->firstCheckCycle = cpu->curCycle();
    }
    if (element->stream->getStreamType() == "store") {
      /**
       * Basically this is a stream store.
       * Make sure the stored element is AddrReady.
       */
      assert(inst->getInstName() == "stream-store" &&
             "Only StreamStore should have usage of store stream element.");
      if (!element->isAddrReady) {
        ready = false;
      }
      continue;
    }
    if (!element->isValueReady) {
      ready = false;
    }
  }

  return ready;
}

void StreamEngine::executeStreamUser(LLVMDynamicInst *inst) {
  assert(this->userElementMap.count(inst) != 0);
}

void StreamEngine::commitStreamUser(LLVMDynamicInst *inst) {
  assert(this->userElementMap.count(inst) != 0);
  // Remove the entry from the elementUserMap.
  for (auto element : this->userElementMap.at(inst)) {
    assert(this->elementUserMap.count(element) != 0);
    auto &userSet = this->elementUserMap.at(element);
    assert(userSet.count(inst) != 0);
    userSet.erase(inst);
  }
  // Remove the entry in the userElementMap.
  this->userElementMap.erase(inst);
}

void StreamEngine::dispatchStreamEnd(StreamEndInst *inst) {
  const auto &endStreamIds = inst->getTDG().stream_end().stream_ids();

  /**
   * Dedup the coalesced stream ids.
   */
  std::unordered_set<Stream *> endedStreams;
  for (auto iter = endStreamIds.rbegin(), end = endStreamIds.rend();
       iter != end; ++iter) {
    // Release in reverse order.
    auto streamId = *iter;
    auto S = this->getStream(streamId);
    if (endedStreams.count(S) != 0) {
      continue;
    }
    endedStreams.insert(S);

    assert(S->configured && "Stream should be configured.");

    /**
     * 1. Step one element (retain one last element).
     * 2. Release all unstepped allocated element.
     * 3. Mark the stream to be unconfigured.
     */

    // 1. Step one element.
    assert(S->allocSize > S->stepSize &&
           "Should have at least one unstepped allocate element.");
    S->stepped = S->stepped->next;
    S->stepSize++;

    // 2. Release allocated but unstepped elements.
    while (S->allocSize > S->stepSize) {
      assert(S->stepped->next != nullptr && "Missing next element.");
      auto releaseElement = S->stepped->next;
      S->stepped->next = releaseElement->next;
      S->allocSize--;
      if (S->head == releaseElement) {
        S->head = S->stepped;
      }
      this->addFreeElement(releaseElement);
    }

    // 3. Mark the stream to be unconfigured.
    S->configured = false;
    if (isDebugStream(S)) {
      debugStream(S, "Dispatch End");
    }
  }
}

void StreamEngine::commitStreamEnd(StreamEndInst *inst) {
  const auto &endStreamIds = inst->getTDG().stream_end().stream_ids();

  /**
   * Deduplicate the streams due to coalescing.
   */
  std::unordered_set<Stream *> endedStreams;
  for (auto iter = endStreamIds.rbegin(), end = endStreamIds.rend();
       iter != end; ++iter) {
    // Release in reverse order.
    auto streamId = *iter;
    auto S = this->getStream(streamId);
    if (endedStreams.count(S) != 0) {
      continue;
    }
    endedStreams.insert(S);

    /**
     * Release the last element we stepped at dispatch.
     */
    this->releaseElement(S);
    if (isDebugStream(S)) {
      debugStream(S, "Commit End");
    }

    /**
     * Check if this stream is offloaded and if so, send the StreamEnd packet.
     */
    assert(!S->dynamicInstanceStates.empty() &&
           "Failed to find ended DynamicInstanceState.");
    auto &endedDynamicInstanceState = S->dynamicInstanceStates.front();
    if (endedDynamicInstanceState.offloadedToCache) {
      auto endedDynamicStreamId =
          new DynamicStreamId(endedDynamicInstanceState.dynamicStreamId);
      // The target address is just virtually 0 (should be set by MLC stream
      // engine).
      auto pkt = TDGPacketHandler::createStreamControlPacket(
          cpu->translateAndAllocatePhysMem(0), cpu->getDataMasterID(), 0,
          MemCmd::Command::StreamEndReq,
          reinterpret_cast<uint64_t>(endedDynamicStreamId));
      DPRINTF(RubyStream, "[%s] Create StreamEnd pkt.\n",
              S->getStreamName().c_str());
      cpu->sendRequest(pkt);
    }

    S->dynamicInstanceStates.pop_front();

    // Notify the stream.
    S->commitStreamEnd(inst);
  }

  this->allocateElements();
}

bool StreamEngine::canStreamStoreDispatch(const StreamStoreInst *inst) const {
  /**
   * * The only requirement about the SQ is already handled.
   */
  return true;
}

std::list<std::unique_ptr<GemForgeSQCallback>>
StreamEngine::createStreamStoreSQCallbacks(StreamStoreInst *inst) {
  std::list<std::unique_ptr<GemForgeSQCallback>> callbacks;
  if (!this->enableLSQ) {
    return callbacks;
  }
  // Find the element to be stored.
  StreamElement *storeElement = nullptr;
  auto storeStream = this->getStream(inst->getTDG().stream_store().stream_id());
  for (auto element : this->userElementMap.at(inst)) {
    if (element == nullptr) {
      continue;
    }
    if (element->stream == storeStream) {
      // Found it.
      storeElement = element;
      break;
    }
  }
  assert(storeElement != nullptr && "Failed to found the store element.");
  callbacks.emplace_back(
      new GemForgeStreamEngineSQCallback(storeElement, inst));
  return callbacks;
}

void StreamEngine::dispatchStreamStore(StreamStoreInst *inst) {
  // So far we just do nothing.
}

void StreamEngine::executeStreamStore(StreamStoreInst *inst) {
  assert(this->userElementMap.count(inst) != 0);
  // Check my element.
  auto storeStream = this->getStream(inst->getTDG().stream_store().stream_id());
  for (auto element : this->userElementMap.at(inst)) {
    if (element == nullptr) {
      continue;
    }
    if (element->stream == storeStream) {
      // Found it.
      element->stored = true;
      // Mark the stored element value ready.
      if (!element->isValueReady) {
        element->markValueReady();
      }
      break;
    }
  }
}

void StreamEngine::commitStreamStore(StreamStoreInst *inst) {
  if (!this->enableLSQ) {
    return;
  }
}

bool StreamEngine::handle(LLVMDynamicInst *inst) { return false; }

void StreamEngine::initializeStreams(
    const ::LLVM::TDG::StreamRegion &streamRegion) {
  // Coalesced streams.
  std::unordered_map<int, CoalescedStream *> coalescedGroupToStreamMap;

  Stream::StreamArguments args;
  args.cpu = cpu;
  args.se = this;
  args.maxSize = this->maxRunAHeadLength;
  args.streamRegion = &streamRegion;

  // Sanity check that we do not have too many streams.
  auto totalAliveStreams = this->enableCoalesce
                               ? streamRegion.total_alive_coalesced_streams()
                               : streamRegion.total_alive_streams();
  if (totalAliveStreams * this->maxRunAHeadLength >
      this->maxTotalRunAheadLength) {
    // If there are too many streams, we reduce the maxSize.
    args.maxSize = this->maxTotalRunAheadLength / totalAliveStreams;
    if (args.maxSize < 3) {
      panic("Too many streams %s TotalAliveStreams %d, FIFOSize %d.\n",
            streamRegion.region().c_str(), totalAliveStreams,
            this->maxTotalRunAheadLength);
    }
  }

  std::vector<Stream *> createdStreams;
  for (const auto &streamInfo : streamRegion.streams()) {
    const auto &streamId = streamInfo.id();
    assert(this->streamMap.count(streamId) == 0 &&
           "Stream is already initialized.");
    auto coalesceGroup = streamInfo.coalesce_group();

    // Set per stream field in stream args.
    args.staticId = streamId;
    args.name = streamInfo.name().c_str();

    if (coalesceGroup != -1 && this->enableCoalesce) {
      // First check if we have created the coalesced stream for the group.
      if (coalescedGroupToStreamMap.count(coalesceGroup) == 0) {
        auto newCoalescedStream = new CoalescedStream(args, streamInfo);
        createdStreams.push_back(newCoalescedStream);
        this->streamMap.emplace(streamId, newCoalescedStream);
        this->coalescedStreamIdMap.emplace(streamId, streamId);
        coalescedGroupToStreamMap.emplace(coalesceGroup, newCoalescedStream);
        hack("Initialized stream %lu %s.\n", streamId,
             newCoalescedStream->getStreamName().c_str());
      } else {
        // This is not the first time we encounter this coalesce group.
        // Add the config to the coalesced stream.
        auto coalescedStream = coalescedGroupToStreamMap.at(coalesceGroup);
        auto coalescedStreamId = coalescedStream->getCoalesceStreamId();
        coalescedStream->addStreamInfo(streamInfo);
        this->coalescedStreamIdMap.emplace(streamId, coalescedStreamId);
        hack("Add coalesced stream %lu %lu %s.\n", streamId, coalescedStreamId,
             coalescedStream->getStreamName().c_str());
      }

      // panic("Disabled stream coalesce so far.");
    } else {
      // Single stream can be immediately constructed and inserted into the
      // map.
      auto newStream = new SingleStream(args, streamInfo);
      createdStreams.push_back(newStream);
      this->streamMap.emplace(streamId, newStream);
      hack("Initialized stream %lu %s.\n", streamId,
           newStream->getStreamName().c_str());
    }
  }

  for (auto newStream : createdStreams) {
    // Initialize any back-edge base stream dependepce.
    newStream->initializeBackBaseStreams();
  }
}

Stream *StreamEngine::getStream(uint64_t streamId) const {
  if (this->coalescedStreamIdMap.count(streamId)) {
    streamId = this->coalescedStreamIdMap.at(streamId);
  }
  auto iter = this->streamMap.find(streamId);
  if (iter == this->streamMap.end()) {
    panic("Failed to find stream %lu.\n", streamId);
  }
  return iter->second;
}

void StreamEngine::tick() {
  this->allocateElements();
  this->issueElements();
  if (curTick() % 10000 == 0) {
    this->updateAliveStatistics();
  }
  if (this->blockCycle > 0) {
    this->blockCycle--;
  }
}

void StreamEngine::updateAliveStatistics() {
  int totalAliveElements = 0;
  int totalAliveMemStreams = 0;
  std::unordered_set<Addr> totalAliveCacheBlocks;
  this->numRunAHeadLengthDist.reset();
  for (const auto &streamPair : this->streamMap) {
    const auto &stream = streamPair.second;
    if (stream->isMemStream()) {
      this->numRunAHeadLengthDist.sample(stream->allocSize);
    }
    if (!stream->configured) {
      continue;
    }
    if (stream->isMemStream()) {
      totalAliveMemStreams++;
    }
  }
  this->numTotalAliveElements.sample(totalAliveElements);
  this->numTotalAliveCacheBlocks.sample(totalAliveCacheBlocks.size());
  this->numTotalAliveMemStreams.sample(totalAliveMemStreams);
}

void StreamEngine::initializeFIFO(size_t totalElements) {
  panic_if(!this->FIFOArray.empty(), "FIFOArray has already been initialized.");

  this->FIFOArray.reserve(totalElements);
  while (this->FIFOArray.size() < totalElements) {
    this->FIFOArray.emplace_back(this);
  }
  this->FIFOFreeListHead = nullptr;
  this->numFreeFIFOEntries = 0;
  for (auto &element : this->FIFOArray) {
    this->addFreeElement(&element);
  }
}

void StreamEngine::addFreeElement(StreamElement *element) {
  element->clear();
  element->next = this->FIFOFreeListHead;
  this->FIFOFreeListHead = element;
  this->numFreeFIFOEntries++;
}

StreamElement *StreamEngine::removeFreeElement() {
  assert(this->hasFreeElement() && "No free element to remove.");
  auto newElement = this->FIFOFreeListHead;
  this->FIFOFreeListHead = this->FIFOFreeListHead->next;
  this->numFreeFIFOEntries--;
  newElement->clear();
  return newElement;
}

bool StreamEngine::hasFreeElement() const {
  return this->numFreeFIFOEntries > 0;
}

const std::list<Stream *> &
StreamEngine::getStepStreamList(Stream *stepS) const {
  assert(stepS != nullptr && "stepS is nullptr.");
  if (this->memorizedStreamStepListMap.count(stepS) != 0) {
    return this->memorizedStreamStepListMap.at(stepS);
  }
  // Create the list.
  std::list<Stream *> stepList;
  std::list<Stream *> stack;
  std::unordered_map<Stream *, int> stackStatusMap;
  stack.emplace_back(stepS);
  stackStatusMap.emplace(stepS, 0);
  while (!stack.empty()) {
    auto S = stack.back();
    if (stackStatusMap.at(S) == 0) {
      // First time.
      for (auto depS : S->dependentStreams) {
        if (depS->getLoopLevel() != stepS->getLoopLevel()) {
          continue;
        }
        if (stackStatusMap.count(depS) != 0) {
          if (stackStatusMap.at(depS) == 1) {
            // Cycle dependence found.
            panic("Cycle dependence found %s.", depS->getStreamName().c_str());
          } else if (stackStatusMap.at(depS) == 2) {
            // This one has already dumped.
            continue;
          }
        }
        stack.emplace_back(depS);
        stackStatusMap.emplace(depS, 0);
      }
      stackStatusMap.at(S) = 1;
    } else if (stackStatusMap.at(S) == 1) {
      // Second time.
      stepList.emplace_front(S);
      stack.pop_back();
      stackStatusMap.at(S) = 2;
    } else {
      // Third time, ignore it as the stream is already in the list.
      stack.pop_back();
    }
  }

  return this->memorizedStreamStepListMap
      .emplace(std::piecewise_construct, std::forward_as_tuple(stepS),
               std::forward_as_tuple(stepList))
      .first->second;
}

void StreamEngine::allocateElements() {
  /**
   * Try to allocate more elements for configured streams.
   * Set a target, try to make sure all streams reach this target.
   * Then increment the target.
   */
  std::vector<Stream *> configuredStepRootStreams;
  for (const auto &IdStream : this->streamMap) {
    auto S = IdStream.second;
    if (S->stepRootStream == S && S->configured) {
      // This is a StepRootStream.
      configuredStepRootStreams.push_back(S);
    }
  }

  // Sort by the allocated size.
  std::sort(configuredStepRootStreams.begin(), configuredStepRootStreams.end(),
            [](Stream *SA, Stream *SB) -> bool {
              return SA->allocSize < SB->allocSize;
            });

  for (auto stepStream : configuredStepRootStreams) {

    /**
     * ! A hack here to delay the allocation if the back base stream has
     * ! not caught up.
     */
    auto maxAllocSize = stepStream->maxSize;
    if (!stepStream->backBaseStreams.empty()) {
      if (stepStream->FIFOIdx.entryIdx > 0) {
        // This is not the first element.
        for (auto backBaseS : stepStream->backBaseStreams) {
          if (backBaseS->stepRootStream == stepStream) {
            // ! This is acutally a pointer chasing pattern.
            // ! No constraint should be enforced here.
            continue;
          }
          if (backBaseS->stepRootStream == nullptr) {
            // ! THis is actually a constant load.
            // ! So far ignore this dependence.
            continue;
          }
          auto backBaseSAllocDiff = backBaseS->allocSize - backBaseS->stepSize;
          auto stepStreamAllocDiff = maxAllocSize - stepStream->stepSize;
          if (backBaseSAllocDiff < stepStreamAllocDiff) {
            // The back base stream is lagging off.
            // Reduce the maxAllocSize.
            maxAllocSize = stepStream->stepSize + backBaseSAllocDiff;
          }
        }
      }
    }

    const auto &stepStreams = this->getStepStreamList(stepStream);
    if (isDebugStream(stepStream)) {
      hack("Try to allocate for debug stream, maxAllocSize %d.", maxAllocSize);
    }
    for (size_t targetSize = 1;
         targetSize <= maxAllocSize && this->hasFreeElement(); ++targetSize) {
      for (auto S : stepStreams) {
        if (isDebugStream(stepStream)) {
          debugStream(S, "Try to allocate for it.");
        }
        if (!this->hasFreeElement()) {
          break;
        }
        if (!S->configured) {
          continue;
        }
        if (S->allocSize >= targetSize) {
          continue;
        }
        if (S != stepStream) {
          if (S->allocSize - S->stepSize >=
              stepStream->allocSize - stepStream->stepSize) {
            // It doesn't make sense to allocate ahead than the step root.
            continue;
          }
        }
        // if (isDebugStream(S)) {
        //   debugStream(S, "allocate one.");
        // }
        this->allocateElement(S);
      }
    }
  }
}

bool StreamEngine::areBaseElementAllocated(Stream *S) {
  // Find the base element.
  for (auto baseS : S->baseStreams) {
    if (baseS->getLoopLevel() != S->getLoopLevel()) {
      continue;
    }

    auto allocated = true;
    if (baseS->stepRootStream == S->stepRootStream) {
      if (baseS->allocSize - baseS->stepSize <= S->allocSize - S->stepSize) {
        // The base stream has not allocate the element we want.
        allocated = false;
      }
    } else {
      // The other one must be a constant stream.
      assert(baseS->stepRootStream == nullptr &&
             "Should be a constant stream.");
      if (baseS->stepped->next == nullptr) {
        allocated = false;
      }
    }
    // hack("Check base element from stream %s for stream %s allocated %d.\n",
    //      baseS->getStreamName().c_str(), S->getStreamName().c_str(),
    //      allocated);
    if (!allocated) {
      return false;
    }
  }
  return true;
}

void StreamEngine::allocateElement(Stream *S) {
  assert(this->hasFreeElement());
  assert(S->configured && "Stream should be configured to allocate element.");
  auto newElement = this->removeFreeElement();
  this->numElementsAllocated++;
  S->statistic.numAllocated++;
  if (S->getStreamType() == "load") {
    this->numLoadElementsAllocated++;
  } else if (S->getStreamType() == "store") {
    this->numStoreElementsAllocated++;
  }

  newElement->stream = S;
  /**
   * next() is called after assign to make sure
   * entryIdx starts from 0.
   */
  newElement->FIFOIdx = S->FIFOIdx;
  S->FIFOIdx.next();

  // Find the base element.
  for (auto baseS : S->baseStreams) {
    if (baseS->getLoopLevel() != S->getLoopLevel()) {
      continue;
    }

    if (baseS->stepRootStream == S->stepRootStream) {
      if (baseS->allocSize - baseS->stepSize <= S->allocSize - S->stepSize) {
        this->dumpFIFO();
        panic("Base %s has not enough allocated element for %s.",
              baseS->getStreamName().c_str(), S->getStreamName().c_str());
      }

      auto baseElement = baseS->stepped;
      auto element = S->stepped;
      while (element != nullptr) {
        assert(baseElement != nullptr && "Failed to find base element.");
        element = element->next;
        baseElement = baseElement->next;
      }
      assert(baseElement != nullptr && "Failed to find base element.");
      newElement->baseElements.insert(baseElement);
    } else {
      // The other one must be a constant stream.
      assert(baseS->stepRootStream == nullptr &&
             "Should be a constant stream.");
      assert(baseS->stepped->next != nullptr && "Missing base element.");
      newElement->baseElements.insert(baseS->stepped->next);
    }
  }

  // Find the back base element, starting from the second element.
  if (newElement->FIFOIdx.entryIdx > 1) {
    for (auto backBaseS : S->backBaseStreams) {
      if (backBaseS->getLoopLevel() != S->getLoopLevel()) {
        continue;
      }

      if (backBaseS->stepRootStream != nullptr) {
        // Try to find the previous element for the base.
        auto baseElement = backBaseS->stepped;
        auto element = S->stepped->next;
        while (element != nullptr) {
          if (baseElement == nullptr) {
            STREAM_ELEMENT_PANIC(newElement,
                                 "Failed to find back base element from %s.\n",
                                 backBaseS->getStreamName().c_str());
          }
          element = element->next;
          baseElement = baseElement->next;
        }
        if (baseElement == nullptr) {
          STREAM_ELEMENT_PANIC(newElement,
                               "Failed to find back base element from %s.\n",
                               backBaseS->getStreamName().c_str());
        }
        // ! Try to check the base element should have the previous element.
        STREAM_ELEMENT_DPRINTF(baseElement, "Consumer for back dependence.\n");
        if (baseElement->FIFOIdx.streamId.streamInstance ==
            newElement->FIFOIdx.streamId.streamInstance) {
          if (baseElement->FIFOIdx.entryIdx + 1 ==
              newElement->FIFOIdx.entryIdx) {
            STREAM_ELEMENT_DPRINTF(newElement, "Found back dependence.\n");
            newElement->baseElements.insert(baseElement);
          } else {
            // STREAM_ELEMENT_PANIC(
            //     newElement, "The base element has wrong FIFOIdx.\n");
          }
        } else {
          // STREAM_ELEMENT_PANIC(newElement,
          //                      "The base element has wrong
          //                      streamInstance.\n");
        }

      } else {
        // ! Should be a constant stream. So far we ignore it.
      }
    }
  }

  newElement->allocateCycle = cpu->curCycle();

  // Create all the cache lines this element will touch.
  if (S->isMemStream()) {
    S->prepareNewElement(newElement);
    const int cacheBlockSize = cpu->system->cacheLineSize();

    for (int currentSize, totalSize = 0; totalSize < newElement->size;
         totalSize += currentSize) {
      if (newElement->cacheBlocks >= StreamElement::MAX_CACHE_BLOCKS) {
        panic("More than %d cache blocks for one stream element, address %lu "
              "size %lu.",
              newElement->cacheBlocks, newElement->addr, newElement->size);
      }
      auto currentAddr = newElement->addr + totalSize;
      currentSize = newElement->size - totalSize;
      // Make sure we don't span across multiple cache blocks.
      if (((currentAddr % cacheBlockSize) + currentSize) > cacheBlockSize) {
        currentSize = cacheBlockSize - (currentAddr % cacheBlockSize);
      }
      // Create the breakdown.
      auto cacheBlockAddr = currentAddr & (~(cacheBlockSize - 1));
      auto &newCacheBlockBreakdown =
          newElement->cacheBlockBreakdownAccesses[newElement->cacheBlocks];
      newCacheBlockBreakdown.cacheBlockVirtualAddr = cacheBlockAddr;
      newCacheBlockBreakdown.virtualAddr = currentAddr;
      newCacheBlockBreakdown.size = currentSize;
      newElement->cacheBlocks++;
    }

    // Create the CacheBlockInfo for the cache blocks.
    if (this->enableMerge) {
      for (int i = 0; i < newElement->cacheBlocks; ++i) {
        auto cacheBlockAddr =
            newElement->cacheBlockBreakdownAccesses[i].cacheBlockVirtualAddr;
        this->cacheBlockRefMap
            .emplace(std::piecewise_construct,
                     std::forward_as_tuple(cacheBlockAddr),
                     std::forward_as_tuple())
            .first->second.reference++;
      }
    }
  }

  // Append to the list.
  S->head->next = newElement;
  S->allocSize++;
  S->head = newElement;
}

void StreamEngine::releaseElement(Stream *S) {

  /**
   * * This function performs a normal release, i.e. release a stepped
   * element.
   */

  assert(S->stepSize > 0 && "No element to release.");
  auto releaseElement = S->tail->next;

  const bool used =
      releaseElement->firstUserSeqNum != LLVMDynamicInst::INVALID_SEQ_NUM;

  /**
   * Sanity check that all the user are done with this element.
   */
  if (this->elementUserMap.count(releaseElement) != 0) {
    assert(this->elementUserMap.at(releaseElement).empty() &&
           "Some unreleased user instruction.");
  }

  S->statistic.numStepped++;
  if (used) {
    S->statistic.numUsed++;

    /**
     * Since this element is used by the core, we update the statistic
     * of the latency of this element experienced by the core.
     */
    if (releaseElement->valueReadyCycle < releaseElement->firstCheckCycle) {
      // The element is ready earlier than core's user.
      auto earlyCycles =
          releaseElement->firstCheckCycle - releaseElement->valueReadyCycle;
      S->statistic.numCoreEarlyElement++;
      S->statistic.numCycleCoreEarlyElement += earlyCycles;
    } else {
      // The element makes the core's user wait.
      auto lateCycles =
          releaseElement->valueReadyCycle - releaseElement->firstCheckCycle;
      S->statistic.numCoreLateElement++;
      S->statistic.numCycleCoreLateElement += lateCycles;
    }
  }
  if (S->getStreamType() == "load") {
    this->numLoadElementsStepped++;
    if (used) {
      this->numLoadElementsUsed++;
      // Update waited cycle information.
      auto waitedCycles = 0;
      if (releaseElement->valueReadyCycle > releaseElement->firstCheckCycle) {
        waitedCycles =
            releaseElement->valueReadyCycle - releaseElement->firstCheckCycle;
      }
      this->numLoadElementWaitCycles += waitedCycles;
    }
  } else if (S->getStreamType() == "store") {
    this->numStoreElementsStepped++;
    if (used) {
      this->numStoreElementsUsed++;
    }
  }

  // Decrease the reference count of the cache blocks.
  if (this->enableMerge) {
    for (int i = 0; i < releaseElement->cacheBlocks; ++i) {
      auto cacheBlockVirtualAddr =
          releaseElement->cacheBlockBreakdownAccesses[i].cacheBlockVirtualAddr;
      auto &cacheBlockInfo = this->cacheBlockRefMap.at(cacheBlockVirtualAddr);
      if (used) {
        cacheBlockInfo.used = true;
      }
      cacheBlockInfo.reference--;
      if (cacheBlockInfo.reference == 0) {
        // Remember to remove the pendingAccesses.
        for (auto &pendingAccess : cacheBlockInfo.pendingAccesses) {
          pendingAccess->handleStreamEngineResponse();
        }
        if (cacheBlockInfo.used && cacheBlockInfo.requestedByLoad) {
          this->numLoadCacheLineUsed++;
        }
        this->cacheBlockRefMap.erase(cacheBlockVirtualAddr);
      }
    }
  }

  S->tail->next = releaseElement->next;
  if (S->stepped == releaseElement) {
    S->stepped = S->tail;
  }
  if (S->head == releaseElement) {
    S->head = S->tail;
  }
  S->stepSize--;
  S->allocSize--;

  this->addFreeElement(releaseElement);
}

void StreamEngine::issueElements() {
  // Find all ready elements.
  std::vector<StreamElement *> readyElements;
  for (auto &element : this->FIFOArray) {
    if (element.stream == nullptr) {
      // Not allocated, ignore.
      continue;
    }
    if (element.isAddrReady) {
      // We already issued request for this element.
      continue;
    }
    // Check if StreamConfig is already executed.
    if (!element.stream->isStreamConfigureExecuted(
            element.FIFOIdx.configSeqNum)) {
      // This stream is not fully configured yet.
      continue;
    }
    // Check if all the base element are value ready.
    bool ready = true;
    for (const auto &baseElement : element.baseElements) {
      if (baseElement->stream == nullptr) {
        // ! Some bug here that the base element is already released.
        continue;
      }
      if (element.stream->baseStreams.count(baseElement->stream) == 0 &&
          element.stream->backBaseStreams.count(baseElement->stream) == 0) {
        continue;
      }
      if (baseElement->FIFOIdx.entryIdx > element.FIFOIdx.entryIdx) {
        // ! Some bug here that the base element is already used by others.
        // TODO: Better handle all these.
        continue;
      }
      if (!baseElement->isValueReady) {
        ready = false;
        break;
      }
    }
    if (ready) {
      readyElements.emplace_back(&element);
    }
  }

  /**
   * Sort the ready elements by create cycle and relative order within
   * the single stream.
   */
  // Sort the ready elements, by their create cycle.
  std::sort(readyElements.begin(), readyElements.end(),
            [](const StreamElement *A, const StreamElement *B) -> bool {
              if (B->allocateCycle > A->allocateCycle) {
                return true;
              } else if (B->stream == A->stream) {
                const auto &AIdx = A->FIFOIdx;
                const auto &BIdx = B->FIFOIdx;
                return BIdx > AIdx;
              } else {
                return false;
              }
            });
  for (auto &element : readyElements) {
    element->isAddrReady = true;
    element->addrReadyCycle = cpu->curCycle();
    if (element->stream->isMemStream()) {
      this->issueElement(element);
    } else {
      // This is an IV stream with back dependence.
      element->markValueReady();
    }
  }
}
void StreamEngine::fetchedCacheBlock(Addr cacheBlockVirtualAddr,
                                     StreamMemAccess *memAccess) {
  // Check if we still have the cache block.
  if (!this->enableMerge) {
    return;
  }
  if (this->cacheBlockRefMap.count(cacheBlockVirtualAddr) == 0) {
    return;
  }
  auto &cacheBlockInfo = this->cacheBlockRefMap.at(cacheBlockVirtualAddr);
  cacheBlockInfo.status = CacheBlockInfo::Status::FETCHED;
  // Notify all the pending streams.
  for (auto &pendingMemAccess : cacheBlockInfo.pendingAccesses) {
    assert(pendingMemAccess != memAccess &&
           "pendingMemAccess should not be fetching access.");
    pendingMemAccess->handleStreamEngineResponse();
  }
  // Remember to clear the pendingAccesses, as they are now released.
  cacheBlockInfo.pendingAccesses.clear();
}

void StreamEngine::issueElement(StreamElement *element) {
  assert(element->isAddrReady && "Address should be ready.");

  assert(element->stream->isMemStream() &&
         "Should never issue element for IVStream.");

  STREAM_ELEMENT_DPRINTF(element, "Issue.\n");

  auto S = element->stream;
  if (S->getStreamType() == "load") {
    this->numLoadElementsFetched++;
    S->statistic.numFetched++;
  }

  if (element->cacheBlocks > 1) {
    STREAM_ELEMENT_PANIC(element, "More than one cache block per element.\n");
  }

  /**
   * A quick hack to coalesce continuous elements that complemently overlap.
   */
  if (this->coalesceContinuousDirectLoadStreamElement(element)) {
    // This is coalesced. Do not issue request to memory.
    if (element->FIFOIdx.streamId.staticId == 34710992 &&
        element->FIFOIdx.streamId.streamInstance == 252 &&
        element->FIFOIdx.entryIdx == 0) {
      hack("Skipped due to coalesced.\n");
    }
    return;
  }

  for (size_t i = 0; i < element->cacheBlocks; ++i) {
    const auto &cacheBlockBreakdown = element->cacheBlockBreakdownAccesses[i];
    auto cacheBlockVirtualAddr = cacheBlockBreakdown.cacheBlockVirtualAddr;

    if (this->enableMerge) {
      // Check if we already have the cache block fetched.
      auto &cacheBlockInfo = this->cacheBlockRefMap.at(cacheBlockVirtualAddr);

      // Mark this line is requested by a load, not a store.
      if (S->getStreamType() == "load") {
        // This line is going to be fetched.
        if (!cacheBlockInfo.requestedByLoad) {
          cacheBlockInfo.requestedByLoad = true;
          this->numLoadCacheLineFetched++;
        }
      }

      if (cacheBlockInfo.status == CacheBlockInfo::Status::FETCHED) {
        // This cache block is already fetched.
        if (element->FIFOIdx.streamId.staticId == 34710992 &&
            element->FIFOIdx.streamId.streamInstance == 252 &&
            element->FIFOIdx.entryIdx == 0) {
          hack("Skipped due to fetched.\n");
        }
        continue;
      }

      if (cacheBlockInfo.status == CacheBlockInfo::Status::FETCHING) {
        // This cache block is already fetching.
        if (element->FIFOIdx.streamId.staticId == 34710992 &&
            element->FIFOIdx.streamId.streamInstance == 252 &&
            element->FIFOIdx.entryIdx == 0) {
          hack("Skipped due to fetching.\n");
        }
        auto memAccess = element->allocateStreamMemAccess(cacheBlockBreakdown);
        if (S->getStreamType() == "load") {
          element->inflyMemAccess.insert(memAccess);
        }
        cacheBlockInfo.pendingAccesses.push_back(memAccess);
        continue;
      }

      if (this->enableStreamPlacement) {
        // This means we have the placement manager.
        if (this->streamPlacementManager->access(cacheBlockBreakdown,
                                                 element)) {
          // Stream placement manager handles this packet.
          // But we need to mark the cache block to be FETCHING.
          cacheBlockInfo.status = CacheBlockInfo::Status::FETCHING;
          // The request is issued by the placement manager.
          S->statistic.numIssuedRequest++;
          continue;
        }
      }
    }

    // Normal case: really fetching this from the cache,
    // i.e. not merged & not handled by placement manager.
    auto vaddr = cacheBlockBreakdown.virtualAddr;
    auto packetSize = cacheBlockBreakdown.size;
    Addr paddr;
    if (cpu->isStandalone()) {
      paddr = cpu->translateAndAllocatePhysMem(vaddr);
    } else {
      panic("Stream so far can only work in standalone mode.");
    }

    // Allocate the book-keeping StreamMemAccess.
    auto memAccess = element->allocateStreamMemAccess(cacheBlockBreakdown);
    auto pkt = TDGPacketHandler::createTDGPacket(
        paddr, packetSize, memAccess, nullptr, cpu->getDataMasterID(), 0, 0);
    S->statistic.numIssuedRequest++;
    if (element->FIFOIdx.streamId.staticId == 34710992 &&
        element->FIFOIdx.streamId.streamInstance == 2523 &&
        element->FIFOIdx.entryIdx == 2) {
      hack("Normally fetched.\n");
    }
    cpu->sendRequest(pkt);

    // Change to FETCHING status.
    if (this->enableMerge) {
      auto &cacheBlockInfo = this->cacheBlockRefMap.at(cacheBlockVirtualAddr);
      cacheBlockInfo.status = CacheBlockInfo::Status::FETCHING;
    }

    if (S->getStreamType() == "load") {
      element->inflyMemAccess.insert(memAccess);
    }
  }

  if (S->getStreamType() != "store" && element->inflyMemAccess.empty()) {
    if (!element->isValueReady) {
      // The element may be already ready as we are issue packets for
      // committed store stream elements.
      element->markValueReady();
    }
  }
}

void StreamEngine::writebackElement(StreamElement *element,
                                    StreamStoreInst *inst) {
  assert(element->isAddrReady && "Address should be ready.");
  auto S = element->stream;
  assert(S->getStreamType() == "store" &&
         "Should never writeback element for non store stream.");

  // Check the bookkeeping for infly writeback memory accesses.
  assert(element->inflyWritebackMemAccess.count(inst) == 0 &&
         "This StreamStoreInst has already been writebacked.");
  auto &inflyWritebackMemAccesses =
      element->inflyWritebackMemAccess
          .emplace(std::piecewise_construct, std::forward_as_tuple(inst),
                   std::forward_as_tuple())
          .first->second;

  STREAM_ELEMENT_DPRINTF(element, "Writeback.\n");

  // hack("Send packt for stream %s.\n", S->getStreamName().c_str());

  for (size_t i = 0; i < element->cacheBlocks; ++i) {
    const auto &cacheBlockBreakdown = element->cacheBlockBreakdownAccesses[i];

    // Translate the virtual address.
    auto vaddr = cacheBlockBreakdown.virtualAddr;
    auto packetSize = cacheBlockBreakdown.size;
    Addr paddr;
    if (cpu->isStandalone()) {
      paddr = cpu->translateAndAllocatePhysMem(vaddr);
    } else {
      panic("Stream so far can only work in standalone mode.");
    }

    if (this->enableStreamPlacement) {
      // This means we have the placement manager.
      if (this->streamPlacementManager->access(cacheBlockBreakdown, element,
                                               true)) {
        // Stream placement manager handles this packet.
        continue;
      }
    }

    // Allocate the book-keeping StreamMemAccess.
    auto memAccess = element->allocateStreamMemAccess(cacheBlockBreakdown);
    inflyWritebackMemAccesses.insert(memAccess);
    // Create the writeback package.
    auto pkt = TDGPacketHandler::createTDGPacket(paddr, packetSize, memAccess,
                                                 this->writebackCacheLine,
                                                 cpu->getDataMasterID(), 0, 0);
    cpu->sendRequest(pkt);
  }
}

void StreamEngine::dumpFIFO() const {
  inform("Total elements %d, free %d, totalRunAhead %d\n",
         this->FIFOArray.size(), this->numFreeFIFOEntries,
         this->getTotalRunAheadLength());

  for (const auto &IdStream : this->streamMap) {
    auto S = IdStream.second;
    if (S->configured) {
      debugStreamWithElements(S, "dump");
    }
  }
}

void StreamEngine::dumpUser() const {
  for (const auto &userElement : this->userElementMap) {
    auto user = userElement.first;
    user->dumpBasic();
    inform("--used element.\n");
    for (auto element : userElement.second) {
      element->dump();
    }
  }
}

void StreamEngine::dump() {
  if (this->enableStreamPlacement) {
    this->streamPlacementManager->dumpCacheStreamAwarePortStatus();
  }
  this->dumpFIFO();
  this->dumpUser();
}

void StreamEngine::exitDump() const {
  if (streamPlacementManager != nullptr) {
    this->streamPlacementManager->dumpStreamCacheStats();
  }
  std::vector<Stream *> allStreams;
  for (auto &pair : this->streamMap) {
    allStreams.push_back(pair.second);
  }
  // Try to sort them.
  std::sort(allStreams.begin(), allStreams.end(),
            [](const Stream *a, const Stream *b) -> bool {
              // Sort by region and then stream name.
              auto aId = a->streamRegion->region() + a->getStreamName();
              auto bId = b->streamRegion->region() + b->getStreamName();
              return aId < bId;
            });
  // Create the stream stats file.
  auto streamStatsFileName =
      "stream.stats." + std::to_string(cpu->cpuId()) + ".txt";
  auto &streamOS = *simout.findOrCreate(streamStatsFileName)->stream();
  for (auto &S : allStreams) {
    S->dumpStreamStats(streamOS);
  }
}

void StreamEngine::throttleStream(Stream *S, StreamElement *element) {
  if (this->throttlingStrategy == ThrottlingStrategyE::STATIC) {
    // Static means no throttling.
    return;
  }
  if (S->getStreamType() == "store") {
    // No need to throttle for store stream.
    return;
  }
  if (element->valueReadyCycle == 0 || element->firstCheckCycle == 0) {
    // No valid cycle record, do nothing.
    return;
  }
  if (element->valueReadyCycle < element->firstCheckCycle) {
    // The element is ready earlier than user, do nothing.
    return;
  }
  // This is a late fetch, increase the counter.
  S->lateFetchCount++;
  if (S->lateFetchCount == 10) {
    // We have reached the threshold to allow the stream to run further ahead.
    auto oldRunAheadSize = S->maxSize;
    /**
     * Get the step root stream.
     * Sometimes, it is possible that stepRootStream is nullptr,
     * which means that this is a constant stream.
     * We do not throttle in this case.
     */
    auto stepRootStream = S->stepRootStream;
    if (stepRootStream != nullptr) {
      const auto &streamList = this->getStepStreamList(stepRootStream);
      if (this->throttlingStrategy == ThrottlingStrategyE::DYNAMIC) {
        // All streams with the same stepRootStream must have the same run
        // ahead length.
        auto totalRunAheadLength = this->getTotalRunAheadLength();
        // Only increase the run ahead length if the totalRunAheadLength is
        // within the 90% of the total FIFO entries. Need better solution
        // here.
        const auto incrementStep = 2;
        if (static_cast<float>(totalRunAheadLength) <
            0.9f * static_cast<float>(this->FIFOArray.size())) {
          for (auto stepS : streamList) {
            // Increase the run ahead length by 2.
            stepS->maxSize += incrementStep;
          }
          assert(S->maxSize == oldRunAheadSize + 2 &&
                 "RunAheadLength is not increased.");
        }
      } else if (this->throttlingStrategy == ThrottlingStrategyE::GLOBAL) {
        this->throttler.throttleStream(S, element);
      }
      // No matter what, just clear the lateFetchCount in the whole step
      // group.
      for (auto stepS : streamList) {
        stepS->lateFetchCount = 0;
      }
    } else {
      // Otherwise, just clear my self.
      S->lateFetchCount = 0;
    }
  }
}

size_t StreamEngine::getTotalRunAheadLength() const {
  size_t totalRunAheadLength = 0;
  for (const auto &IdStream : this->streamMap) {
    auto S = IdStream.second;
    if (!S->configured) {
      continue;
    }
    totalRunAheadLength += S->maxSize;
  }
  return totalRunAheadLength;
}

const ::LLVM::TDG::StreamRegion &
StreamEngine::getStreamRegion(const std::string &relativePath) const {
  if (this->memorizedStreamRegionMap.count(relativePath) != 0) {
    return this->memorizedStreamRegionMap.at(relativePath);
  }

  auto fullPath = cpu->getTraceExtraFolder() + "/" + relativePath;
  ProtoInputStream istream(fullPath);
  auto &protobufRegion =
      this->memorizedStreamRegionMap
          .emplace(std::piecewise_construct,
                   std::forward_as_tuple(relativePath), std::forward_as_tuple())
          .first->second;
  if (!istream.read(protobufRegion)) {
    panic("Failed to read in the stream region from file %s.",
          fullPath.c_str());
  }
  return protobufRegion;
}

/********************************************************************
 * StreamThrottler.
 *
 * When we trying to throttle a stream, the main problem is to avoid
 * deadlock, as we do not reclaim stream element once it is allocated until
 * it is stepped.
 *
 * To avoid deadlock, we leverage the information of total alive streams
 * that can coexist with the current stream, and assign InitMaxSize number of
 * elements to these streams, which is called BasicEntries.
 * * BasicEntries = TotalAliveStreams * InitMaxSize.
 *
 * Then we want to know how many of these BasicEntries is already assigned to
 * streams. This number is called AssignedBasicEntries.
 * * AssignedBasicEntries = CurrentAliveStreams * InitMaxSize.
 *
 * We also want to know the number of AssignedEntries and UnAssignedEntries.
 * * AssignedEntries = Sum(MaxSize, CurrentAliveStreams).
 * * UnAssignedEntries = FIFOSize - AssignedEntries.
 *
 * The available pool for throttling is:
 * * AvailableEntries = \
 * *   UnAssignedEntries - (BasicEntries - AssignedBasicEntries).
 *
 * Also we enforce an upper bound on the entries:
 * * UpperBoundEntries = \
 * *   (FIFOSize - BasicEntries) / StepGroupSize + InitMaxSize.
 *
 * As we are throttling streams altogether with the same stepRoot, the
 *condition is:
 * * AvailableEntries >= IncrementSize * StepGroupSize.
 * * CurrentMaxSize + IncrementSize <= UpperBoundEntries
 *
 ********************************************************************/

StreamEngine::StreamThrottler::StreamThrottler(StreamEngine *_se) : se(_se) {}

void StreamEngine::StreamThrottler::throttleStream(Stream *S,
                                                   StreamElement *element) {
  auto stepRootStream = S->stepRootStream;
  assert(stepRootStream != nullptr &&
         "Do not make sense to throttle for a constant stream.");
  const auto &streamList = this->se->getStepStreamList(stepRootStream);

  // * AssignedEntries.
  auto currentAliveStreams = 0;
  auto assignedEntries = 0;
  for (const auto &IdStream : this->se->streamMap) {
    auto S = IdStream.second;
    if (!S->configured) {
      continue;
    }
    currentAliveStreams++;
    assignedEntries += S->maxSize;
  }
  // * UnAssignedEntries.
  int unAssignedEntries = this->se->maxTotalRunAheadLength - assignedEntries;
  // * BasicEntries.
  auto streamRegion = S->streamRegion;
  int totalAliveStreams = this->se->enableCoalesce
                              ? streamRegion->total_alive_coalesced_streams()
                              : streamRegion->total_alive_streams();
  int basicEntries = std::max(totalAliveStreams, currentAliveStreams) *
                     this->se->maxRunAHeadLength;
  // * AssignedBasicEntries.
  int assignedBasicEntries = currentAliveStreams * this->se->maxRunAHeadLength;
  // * AvailableEntries.
  int availableEntries =
      unAssignedEntries - (basicEntries - assignedBasicEntries);
  // * UpperBoundEntries.
  int upperBoundEntries =
      (this->se->maxTotalRunAheadLength - basicEntries) / streamList.size() +
      this->se->maxRunAHeadLength;
  const auto incrementStep = 2;
  int totalIncrementEntries = incrementStep * streamList.size();

  if (availableEntries < totalIncrementEntries) {
    return;
  }
  if (totalAliveStreams * this->se->maxRunAHeadLength +
          streamList.size() * (stepRootStream->maxSize + incrementStep -
                               this->se->maxRunAHeadLength) >=
      this->se->maxTotalRunAheadLength) {
    return;
  }
  if (stepRootStream->maxSize + incrementStep > upperBoundEntries) {
    return;
  }

  if (isDebugStream(stepRootStream)) {
    inform(
        "AssignedEntries %d UnAssignedEntries %d BasicEntries %d "
        "AssignedBasicEntries %d AvailableEntries %d UpperBoundEntries %d.\n",
        assignedEntries, unAssignedEntries, basicEntries, assignedBasicEntries,
        availableEntries, upperBoundEntries);
  }

  auto oldMaxSize = S->maxSize;
  for (auto stepS : streamList) {
    // Increase the run ahead length by 2.
    stepS->maxSize += incrementStep;
  }
  assert(S->maxSize == oldMaxSize + incrementStep &&
         "RunAheadLength is not increased.");
}

/***********************************************************
 * Callback structures for LSQ.
 ***********************************************************/

bool StreamEngine::GemForgeStreamEngineLQCallback::getAddrSize(Addr &addr,
                                                               uint32_t &size) {
  // Check if the address is ready.
  if (!this->element->isAddrReady) {
    return false;
  }
  addr = this->element->addr;
  size = this->element->size;
  return true;
}

bool StreamEngine::GemForgeStreamEngineLQCallback::isIssued() {
  return cpu->getInflyInstStatus(userInst->getId()) ==
         LLVMTraceCPU::InstStatus::ISSUED;
}

void StreamEngine::GemForgeStreamEngineLQCallback::RAWMisspeculate() {
  cpu->getIEWStage().misspeculateInst(userInst);
}

bool StreamEngine::GemForgeStreamEngineSQCallback::getAddrSize(Addr &addr,
                                                               uint32_t &size) {
  // Check if the address is ready.
  if (!this->element->isAddrReady) {
    return false;
  }
  addr = this->element->addr;
  size = this->element->size;
  return true;
}

void StreamEngine::GemForgeStreamEngineSQCallback::writeback() {
  // Start inform the stream engine to write back.
  this->element->se->writebackElement(this->element, this->storeInst);
}

bool StreamEngine::GemForgeStreamEngineSQCallback::isWritebacked() {
  assert(this->element->inflyWritebackMemAccess.count(this->storeInst) != 0 &&
         "Missing writeback StreamMemAccess?");
  // Check if all the writeback accesses are done.
  return this->element->inflyWritebackMemAccess.at(this->storeInst).empty();
}

void StreamEngine::GemForgeStreamEngineSQCallback::writebacked() {
  // Remember to clear the inflyWritebackStreamAccess.
  assert(this->element->inflyWritebackMemAccess.count(this->storeInst) != 0 &&
         "Missing writeback StreamMemAccess?");
  this->element->inflyWritebackMemAccess.erase(this->storeInst);
  // Remember to change the status of the stream store to committed.
  auto cpu = this->element->se->cpu;
  auto storeInstId = this->storeInst->getId();
  auto status = cpu->getInflyInstStatus(storeInstId);
  assert(status == LLVMTraceCPU::InstStatus::COMMITTING &&
         "Writebacked instructions should be committing.");
  cpu->updateInflyInstStatus(storeInstId, LLVMTraceCPU::InstStatus::COMMITTED);
}

bool StreamEngine::shouldOffloadStream(Stream *S, uint64_t streamInstance) {
  if (!S->isDirectLoadStream() && !S->isPointerChaseLoadStream()) {
    return false;
  }
  /**
   * Make sure we do not offload empty stream.
   * This information may be known at configuration time, or even require oracle
   * information. However, as the stream is empty, trace-based simulation does
   * not know which LLC bank should the stream be offloaded to.
   * TODO: Improve this.
   */
  if (S->getStreamLengthAtInstance(streamInstance) == 0) {
    return false;
  }
  // Let's use the previous staistic of the average stream.
  bool enableSmartDecision = false;
  if (enableSmartDecision) {
    const auto &statistic = S->statistic;
    if (statistic.numConfigured == 0) {
      // First time, maybe we aggressively offload as this is the
      // case for many microbenchmark we designed.
      return true;
    }
    auto avgLength = statistic.numUsed / statistic.numConfigured;
    if (avgLength < 500) {
      return false;
    }
  }

  return true;
}

bool StreamEngine::coalesceContinuousDirectLoadStreamElement(
    StreamElement *element) {
  // Check if this is the first element.
  if (element->FIFOIdx.entryIdx == 0) {
    return false;
  }
  auto S = element->stream;
  if (!S->isDirectLoadStream()) {
    return false;
  }
  // Get the previous element.
  for (auto prevElement = S->tail->next; prevElement != nullptr;
       prevElement = prevElement->next) {
    if (prevElement->next == element) {
      // We found the previous element. Check if completely overlap.
      assert(prevElement->FIFOIdx.entryIdx + 1 == element->FIFOIdx.entryIdx &&
             "Mismatch entryIdx for prevElement.");
      assert(prevElement->FIFOIdx.streamId == element->FIFOIdx.streamId &&
             "Mismatch streamId for prevElement.");
      if (element->cacheBlocks != prevElement->cacheBlocks) {
        // If cache block size does not match, not completely overlapped.
        return false;
      }
      for (int cacheBlockIdx = 0; cacheBlockIdx < element->cacheBlocks;
           ++cacheBlockIdx) {
        const auto &block = element->cacheBlockBreakdownAccesses[cacheBlockIdx];
        const auto &prevBlock =
            prevElement->cacheBlockBreakdownAccesses[cacheBlockIdx];
        if (block.cacheBlockVirtualAddr != prevBlock.cacheBlockVirtualAddr) {
          // Not completely overlapped.
          return false;
        }
      }
      // Completely overlapped. Check if the previous element is already value
      // ready.
      if (prevElement->isValueReady) {
        // Mark value ready immediately.
        element->markValueReady();
      } else {
        // Mark the prevElement to propagate its value ready signal to next
        // element.
        prevElement->markNextElementValueReady = true;
      }
      return true;
    }
  }
  assert(false && "Failed to find the previous element.");
}