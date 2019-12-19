#include "stream_engine.hh"
#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"

#include "base/trace.hh"
#include "debug/RubyStream.hh"
#include "debug/StreamEngine.hh"

namespace {
static std::string DEBUG_STREAM_NAME =
    "(IV acmod.c::1232(acmod_flags2list) bb19 bb19::tmp21(phi))";

bool isDebugStream(Stream *S) {
  return S->getStreamName() == DEBUG_STREAM_NAME;
}

} // namespace

#define SE_DPRINTF(format, args...)                                            \
  DPRINTF(StreamEngine, "[SE%d]: " format, this->cpuDelegator->cpuId(), ##args)

#define DEBUG_TYPE StreamEngine
#include "stream_log.hh"

StreamEngine::StreamEngine(Params *params)
    : GemForgeAccelerator(params), streamPlacementManager(nullptr),
      isOracle(false), writebackCacheLine(nullptr), throttler(this) {

  this->isOracle = params->streamEngineIsOracle;
  this->maxRunAHeadLength = params->streamEngineMaxRunAHeadLength;
  this->currentTotalRunAheadLength = 0;
  this->maxTotalRunAheadLength = params->streamEngineMaxTotalRunAHeadLength;
  // this->maxTotalRunAheadLength = this->maxRunAHeadLength * 512;
  if (params->streamEngineThrottling == "static") {
    this->throttlingStrategy = ThrottlingStrategyE::STATIC;
  } else if (params->streamEngineThrottling == "dynamic") {
    this->throttlingStrategy = ThrottlingStrategyE::DYNAMIC;
  } else {
    this->throttlingStrategy = ThrottlingStrategyE::GLOBAL;
  }
  this->enableLSQ = params->streamEngineEnableLSQ;
  this->enableCoalesce = params->streamEngineEnableCoalesce;
  this->enableMerge = params->streamEngineEnableMerge;
  this->enableStreamPlacement = params->streamEngineEnablePlacement;
  this->enableStreamPlacementOracle = params->streamEngineEnablePlacementOracle;
  this->enableStreamPlacementBus = params->streamEngineEnablePlacementBus;
  this->noBypassingStore = params->streamEngineNoBypassingStore;
  this->continuousStore = params->streamEngineContinuousStore;
  this->enablePlacementPeriodReset = params->streamEnginePeriodReset;
  this->placementLat = params->streamEnginePlacementLat;
  this->placement = params->streamEnginePlacement;
  this->enableStreamFloat = params->streamEngineEnableFloat;
  this->enableStreamFloatIndirect = params->streamEngineEnableFloatIndirect;
  this->streamFloatPolicy = m5::make_unique<StreamFloatPolicy>(
      this->enableStreamFloat, params->streamEngineFloatPolicy);

  this->initializeFIFO(this->maxTotalRunAheadLength);
}

StreamEngine::~StreamEngine() {
  if (this->streamPlacementManager != nullptr) {
    delete this->streamPlacementManager;
    this->streamPlacementManager = nullptr;
  }

  // Clear all the allocated streams.
  for (auto &streamIdStreamPair : this->streamMap) {
    /**
     * Be careful here as CoalescedStream are not newed, no need to delete
     * them.
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

void StreamEngine::handshake(GemForgeCPUDelegator *_cpuDelegator,
                             GemForgeAcceleratorManager *_manager) {
  GemForgeAccelerator::handshake(_cpuDelegator, _manager);

  LLVMTraceCPU *_cpu = nullptr;
  if (auto llvmTraceCPUDelegator =
          dynamic_cast<LLVMTraceCPUDelegator *>(_cpuDelegator)) {
    _cpu = llvmTraceCPUDelegator->cpu;
  }
  // assert(_cpu != nullptr && "Only work for LLVMTraceCPU so far.");
  this->cpu = _cpu;

  this->writebackCacheLine = new uint8_t[cpuDelegator->cacheLineSize()];
  if (this->enableStreamPlacement) {
    this->streamPlacementManager =
        new StreamPlacementManager(cpuDelegator, this);
  }
}

void StreamEngine::regStats() {
  GemForgeAccelerator::regStats();
  assert(this->manager && "No handshake.");

#define scalar(stat, describe)                                                 \
  this->stat.name(this->manager->name() + ("." #stat))                         \
      .desc(describe)                                                          \
      .prereq(this->stat)

  scalar(numConfigured, "Number of streams configured.");
  scalar(numStepped, "Number of streams stepped.");
  scalar(numUnstepped, "Number of streams unstepped.");
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

bool StreamEngine::canStreamConfig(const StreamConfigArgs &args) const {
  /**
   * A stream can be configured iff. we can guarantee that it will be
   * allocate one entry when configured.
   *
   * If this this the first time we encounter the stream, we check the
   * number of free entries. Otherwise, we ALSO ensure that allocSize <
   * maxSize.
   */

  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);
  auto configuredStreams = this->enableCoalesce
                               ? streamRegion.coalesced_stream_ids_size()
                               : streamRegion.streams_size();

  // Sanity check on the number of configured streams.
  {
    if (configuredStreams * 3 > this->maxTotalRunAheadLength) {
      panic("Too many streams configuredStreams for %s %d, FIFOSize %d.\n",
            infoRelativePath.c_str(), configuredStreams,
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
        if (S->getAllocSize() == S->maxSize) {
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
        if (S->getAllocSize() == S->maxSize) {
          // No more quota.
          return false;
        }
      }
    }
  }
  return true;
}

void StreamEngine::dispatchStreamConfig(const StreamConfigArgs &args) {
  assert(this->canStreamConfig(args) && "Cannot configure stream.");

  this->numConfigured++;
  this->numInflyStreamConfigurations++;
  // We require next tick.
  this->manager->scheduleTickNextCycle();
  assert(this->numInflyStreamConfigurations < 100 &&
         "Too many infly StreamConfigurations.");

  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);

  // Initialize all the streams if this is the first time we encounter the
  // loop.
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

  auto configStreams = this->getConfigStreamsInRegion(streamRegion);
  for (auto &S : configStreams) {
    assert(!S->configured && "The stream should not be configured.");
    S->configured = true;
    S->statistic.numConfigured++;

    // Notify the stream.
    S->configure(args.seqNum, args.tc);
  }

  // 3. Allocate one new entries for all streams.
  for (auto S : configStreams) {
    // hack("Allocate element for stream %s.\n",
    // S->getStreamName().c_str());
    assert(this->hasFreeElement());
    assert(S->getAllocSize() < S->maxSize);
    assert(this->areBaseElementAllocated(S));
    this->allocateElement(S);
  }
}

void StreamEngine::executeStreamConfig(const StreamConfigArgs &args) {

  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);

  auto configStreams = this->getConfigStreamsInRegion(streamRegion);
  for (auto &S : configStreams) {
    // Simply notify the stream.
    const StreamConfigArgs::InputVec *inputVec = nullptr;
    if (args.inputMap) {
      inputVec = &(args.inputMap->at(S->staticId));
    }
    S->executeStreamConfig(args.seqNum, inputVec);
    /**
     * StreamAwareCache: Send a StreamConfigReq to the cache hierarchy.
     */
    auto &dynStream = S->getDynamicStream(args.seqNum);
    if (this->streamFloatPolicy->shouldFloatStream(
            S, dynStream.dynamicStreamId.streamInstance)) {

      S->statistic.numFloated++;

      // Remember the offloaded decision.
      // ! Only do this for the root offloaded stream.
      dynStream.offloadedToCache = true;

      // Get the CacheStreamConfigureData.
      auto streamConfigureData = S->allocateCacheConfigureData(args.seqNum);

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
                      dependentStream->allocateCacheConfigureData(args.seqNum));
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
            for (auto indirectStream : backDependentStream->dependentStreams) {
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
                      indirectStream->allocateCacheConfigureData(args.seqNum));
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

      auto pkt = GemForgePacketHandler::createStreamControlPacket(
          streamConfigureData->initPAddr, cpuDelegator->dataMasterId(), 0,
          MemCmd::Command::StreamConfigReq,
          reinterpret_cast<uint64_t>(streamConfigureData));
      DPRINTF(RubyStream,
              "Create StreamConfig pkt %#x %#x, initVAddr: %#x, initPAddr "
              "%#x.\n",
              pkt, streamConfigureData, streamConfigureData->initVAddr,
              streamConfigureData->initPAddr);
      cpuDelegator->sendRequest(pkt);
    }
  }
}

void StreamEngine::commitStreamConfig(const StreamConfigArgs &args) {
  // So far we don't need to do anything.
}

void StreamEngine::rewindStreamConfig(const StreamConfigArgs &args) {

  const auto &infoRelativePath = args.infoRelativePath;
  const auto &configSeqNum = args.seqNum;
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);

  auto configStreams = this->getConfigStreamsInRegion(streamRegion);
  for (auto &S : configStreams) {
    // This file is already too long, move this to stream.cc.
    S->rewindStreamConfig(configSeqNum);
  }

  this->numInflyStreamConfigurations--;
}

bool StreamEngine::canStreamStep(uint64_t stepStreamId) const {
  auto stepStream = this->getStream(stepStreamId);

  bool canStep = true;
  for (auto S : this->getStepStreamList(stepStream)) {
    if (!S->canStep()) {
      canStep = false;
      break;
    }
  }
  return canStep;
}

void StreamEngine::dispatchStreamStep(uint64_t stepStreamId) {
  /**
   * For all the streams get stepped, increase the stepped pointer.
   */

  assert(this->canStreamStep(stepStreamId) &&
         "canStreamStep assertion failed.");
  this->numStepped++;

  auto stepStream = this->getStream(stepStreamId);

  // hack("Step stream %s.\n", stepStream->getStreamName().c_str());

  for (auto S : this->getStepStreamList(stepStream)) {
    assert(S->configured && "Stream should be configured to be stepped.");
    this->stepElement(S);
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

void StreamEngine::commitStreamStep(uint64_t stepStreamId) {
  auto stepStream = this->getStream(stepStreamId);

  const auto &stepStreams = this->getStepStreamList(stepStream);

  for (auto S : stepStreams) {
    /**
     * Why only throttle for streamStep?
     * Normally you want to throttling when you release the element.
     * However, so far the throttling is constrainted by the
     * totalRunAheadLength, which only considers configured streams.
     * Therefore, we can not throttle for the last element (streamEnd), as
     * some base streams may already be cleared, and we get an inaccurate
     * totalRunAheadLength, causing the throttling to exceed the limit and
     * deadlock.
     *
     * To solve this, we only do throttling for streamStep.
     */
    this->releaseElementStepped(S, true /* doThrottle */);
  }

  // ! Do not allocate here.
  // ! allocateElements() will handle it.

  if (isDebugStream(stepStream)) {
  }
}

void StreamEngine::rewindStreamStep(uint64_t stepStreamId) {
  this->numUnstepped++;
  auto stepStream = this->getStream(stepStreamId);
  for (auto S : this->getStepStreamList(stepStream)) {
    assert(S->configured && "Stream should be configured to be stepped.");
    this->unstepElement(S);
  }
}

int StreamEngine::getStreamUserLQEntries(const StreamUserArgs &args) const {
  // Only care this if we enable lsq for the stream engine.
  if (!this->enableLSQ) {
    return 0;
  }

  // Collect all the elements used.
  std::unordered_set<StreamElement *> usedElementSet;
  for (const auto &streamId : args.usedStreamIds) {
    auto S = this->getStream(streamId);
    if (!S->configured) {
      // Ignore the out-of-loop use (see dispatchStreamUser).
      continue;
    }
    auto element = S->getFirstUnsteppedElement();
    usedElementSet.insert(element);
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
    if (element->isFirstUserDispatched()) {
      // Not the first user of the load stream element. Ignore it.
      continue;
    }
    firstUsedLoadStreamElement++;
  }

  return firstUsedLoadStreamElement;
}

int StreamEngine::createStreamUserLQCallbacks(
    const StreamUserArgs &args, GemForgeLQCallbackList &callbacks) {
  auto seqNum = args.seqNum;
  auto &elementSet = this->userElementMap.at(seqNum);
  auto numCallbacks = 0;
  for (auto &element : elementSet) {
    if (element == nullptr) {
      continue;
    }
    if (element->stream->getStreamType() != "load") {
      // Not a load stream.
      continue;
    }
    if (element->firstUserSeqNum == seqNum) {
      // Insert into the load queue if we model the lsq.
      if (this->enableLSQ) {
        assert(numCallbacks < callbacks.size() && "LQCallback overflows.");
        callbacks.at(numCallbacks) =
            m5::make_unique<GemForgeStreamEngineLQCallback>(element, seqNum,
                                                            args.usedStreamIds);
        numCallbacks++;
      }
    }
  }
  return numCallbacks;
}

void StreamEngine::dispatchStreamUser(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  SE_DPRINTF("Dispatch StreamUser %llu.\n", seqNum);
  assert(this->userElementMap.count(seqNum) == 0);

  auto &elementSet =
      this->userElementMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(seqNum),
                   std::forward_as_tuple())
          .first->second;

  for (const auto &streamId : args.usedStreamIds) {
    auto S = this->getStream(streamId);

    /**
     * It is possible that the stream is unconfigured (out-loop use).
     * In such case we assume it's ready and use a nullptr as a special
     * element
     */
    if (!S->configured) {
      elementSet.insert(nullptr);
    } else {
      auto element = S->getFirstUnsteppedElement();
      // Mark the first user sequence number.
      if (!element->isFirstUserDispatched()) {
        element->firstUserSeqNum = seqNum;
        if (S->getStreamType() == "load" && !S->getFloatManual() &&
            element->isAddrReady) {
          // The element should already be in peb, remove it.
          this->peb.removeElement(element);
        }
      }
      elementSet.insert(element);
      // Construct the elementUserMap.
      this->elementUserMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(element),
                   std::forward_as_tuple())
          .first->second.insert(seqNum);
    }
  }
}

bool StreamEngine::areUsedStreamsReady(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  assert(this->userElementMap.count(seqNum) != 0);

  bool ready = true;
  for (auto &element : this->userElementMap.at(seqNum)) {
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
      element->firstCheckCycle = cpuDelegator->curCycle();
    }
    if (element->stream->getStreamType() == "store") {
      /**
       * Basically this is a stream store.
       * Make sure the stored element is AddrReady.
       */
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

void StreamEngine::executeStreamUser(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  assert(this->userElementMap.count(seqNum) != 0);

  if (args.values == nullptr) {
    // This is traced base simulation, and they do not require us to provide
    // the value.
    return;
  }
  std::unordered_map<Stream *, StreamElement *> streamToElementMap;
  for (auto &element : this->userElementMap.at(seqNum)) {
    assert(element && "Out-of-loop use after StreamEnd cannot be handled in "
                      "execution-based simulation.");
    auto inserted = streamToElementMap
                        .emplace(std::piecewise_construct,
                                 std::forward_as_tuple(element->stream),
                                 std::forward_as_tuple(element))
                        .second;
    assert(inserted && "Using two elements from the same stream.");
  }
  for (auto streamId : args.usedStreamIds) {
    /**
     * This is necessary, we can not directly use the usedStreamId cause it
     * may be a coalesced stream.
     */
    auto S = this->getStream(streamId);
    auto element = streamToElementMap.at(S);
    args.values->emplace_back();
    /**
     * Make sure we zero out the data.
     */
    args.values->back().fill(0);
    if (element->stream->getStreamType() == "store") {
      /**
       * This should be a stream store. Just leave it there.
       */
      continue;
    } else {
      /**
       * Read in the value.
       */
      auto vaddr = element->addr;
      int size = element->size;
      if (auto CS = dynamic_cast<CoalescedStream *>(S)) {
        // Handle offset for coalesced stream.
        int32_t offset;
        CS->getCoalescedOffsetAndSize(streamId, offset, size);
        vaddr += offset;
      }
      assert(size <= StreamUserArgs::MaxElementSize &&
             "Do we really have such huge register.");
      element->getValue(vaddr, size, args.values->back().data());
    }
  }
}

void StreamEngine::commitStreamUser(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  SE_DPRINTF("Commit StreamUser %llu.\n", seqNum);
  assert(this->userElementMap.count(seqNum) && "UserElementMap not correct.");
  // Remove the entry from the elementUserMap.
  for (auto element : this->userElementMap.at(seqNum)) {
    /**
     * As a hack, we use nullptr to represent an out-of-loop use.
     * TODO: Fix this.
     */
    if (!element) {
      continue;
    }

    /**
     * Sanity check that no faulted block is used.
     */
    auto S = element->getStream();
    // Dummy way to check the streamId.
    for (auto streamId : args.usedStreamIds) {
      // Check if this streamId corresponding to S.
      if (this->getStream(streamId) != S) {
        continue;
      }
      auto vaddr = element->addr;
      int32_t size = element->size;
      if (auto CS = dynamic_cast<CoalescedStream *>(S)) {
        // Handle offset for coalesced stream.
        int32_t offset;
        CS->getCoalescedOffsetAndSize(streamId, offset, size);
        vaddr += offset;
      }
      if (element->isValueFaulted(vaddr, size)) {
        S_ELEMENT_PANIC(element, "Commit user of faulted value.");
      }
    }

    auto &userSet = this->elementUserMap.at(element);
    assert(userSet.erase(seqNum) && "Not found in userSet.");
  }
  // Remove the entry in the userElementMap.
  this->userElementMap.erase(seqNum);
}

void StreamEngine::rewindStreamUser(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  for (auto element : this->userElementMap.at(seqNum)) {
    // The element should be in unstepped state.
    assert(!element->isStepped && "Rewind user of stepped element.");
    if (element->firstUserSeqNum == seqNum) {
      // I am the first user.
      element->firstUserSeqNum = LLVMDynamicInst::INVALID_SEQ_NUM;
      // Check if the element should go back to PEB.
      if (element->stream->getStreamType() == "load" && element->isAddrReady &&
          !element->stream->getFloatManual()) {
        this->peb.addElement(element);
      }
    }
    // Remove the entry from the elementUserMap.
    auto &userSet = this->elementUserMap.at(element);
    assert(userSet.erase(seqNum) && "Not found in userSet.");
  }
  // Remove the entry in the userElementMap.
  this->userElementMap.erase(seqNum);
}

void StreamEngine::dispatchStreamEnd(const StreamEndArgs &args) {
  const auto &streamRegion = this->getStreamRegion(args.infoRelativePath);
  const auto &endStreamInfos = streamRegion.streams();

  SE_DPRINTF("Dispatch StreamEnd for %s.\n", streamRegion.region().c_str());

  /**
   * Dedup the coalesced stream ids.
   */
  std::unordered_set<Stream *> endedStreams;
  for (auto iter = endStreamInfos.rbegin(), end = endStreamInfos.rend();
       iter != end; ++iter) {
    // Release in reverse order.
    auto streamId = iter->id();
    auto S = this->getStream(streamId);
    if (endedStreams.count(S) != 0) {
      continue;
    }
    endedStreams.insert(S);

    // 1. Step one element.
    this->stepElement(S);

    // 2. Mark the dynamicStream as ended.
    S->dispatchStreamEnd(args.seqNum);
    if (isDebugStream(S)) {
      S_DPRINTF(S, "Dispatch End");
    }
  }
}

void StreamEngine::rewindStreamEnd(const StreamEndArgs &args) {
  const auto &streamRegion = this->getStreamRegion(args.infoRelativePath);
  const auto &endStreamInfos = streamRegion.streams();

  SE_DPRINTF("Rewind StreamEnd for %s.\n", streamRegion.region().c_str());

  /**
   * Dedup the coalesced stream ids.
   */
  std::unordered_set<Stream *> endedStreams;
  for (auto iter = endStreamInfos.rbegin(), end = endStreamInfos.rend();
       iter != end; ++iter) {
    // Rewind in reverse order.
    auto streamId = iter->id();
    auto S = this->getStream(streamId);
    if (endedStreams.count(S) != 0) {
      continue;
    }
    endedStreams.insert(S);

    // 1. Restart the last dynamic stream.
    S->rewindStreamEnd(args.seqNum);

    // 2. Unstep one element.
    this->unstepElement(S);
    if (isDebugStream(S)) {
      S_DPRINTF(S, "Rewind End");
    }
  }
}

void StreamEngine::commitStreamEnd(const StreamEndArgs &args) {

  this->numInflyStreamConfigurations--;
  assert(this->numInflyStreamConfigurations >= 0 &&
         "Negative infly StreamConfigurations.");

  const auto &streamRegion = this->getStreamRegion(args.infoRelativePath);
  const auto &endStreamInfos = streamRegion.streams();

  SE_DPRINTF("Commit StreamEnd for %s.\n", streamRegion.region().c_str());

  /**
   * Deduplicate the streams due to coalescing.
   */
  std::unordered_set<Stream *> endedStreams;
  for (auto iter = endStreamInfos.rbegin(), end = endStreamInfos.rend();
       iter != end; ++iter) {
    // Release in reverse order.
    auto streamId = iter->id();
    auto S = this->getStream(streamId);
    if (endedStreams.count(S) != 0) {
      continue;
    }
    endedStreams.insert(S);

    /**
     * Release all unstepped element until there is none.
     */
    while (this->releaseElementUnstepped(S)) {
    }

    /**
     * Release the last element we stepped at dispatch.
     */
    this->releaseElementStepped(S, false /* doThrottle */);
    if (isDebugStream(S)) {
      S_DPRINTF(S, "Commit End");
    }

    /**
     * Check if this stream is offloaded and if so, send the StreamEnd
     * packet.
     */
    assert(!S->dynamicStreams.empty() &&
           "Failed to find ended DynamicInstanceState.");
    auto &endedDynamicStream = S->dynamicStreams.front();
    if (endedDynamicStream.offloadedToCache) {
      // We need to explicitly allocate and copy the DynamicStreamId for the
      // packet.
      auto endedDynamicStreamId =
          new DynamicStreamId(endedDynamicStream.dynamicStreamId);
      // The target address is just virtually 0 (should be set by MLC stream
      // engine).
      Addr initPAddr = 0;
      auto pkt = GemForgePacketHandler::createStreamControlPacket(
          initPAddr, cpuDelegator->dataMasterId(), 0,
          MemCmd::Command::StreamEndReq,
          reinterpret_cast<uint64_t>(endedDynamicStreamId));
      DPRINTF(RubyStream, "[%s] Create StreamEnd pkt.\n",
              S->getStreamName().c_str());
      cpuDelegator->sendRequest(pkt);
    }

    // Notify the stream.
    S->commitStreamEnd(args.seqNum);
  }

  this->allocateElements();
}

bool StreamEngine::canStreamStoreDispatch(const StreamStoreInst *inst) const {
  /**
   * * The only requirement about the SQ is already handled in the CPU.
   */
  return true;
}

std::list<std::unique_ptr<GemForgeSQCallback>>
StreamEngine::createStreamStoreSQCallbacks(StreamStoreInst *inst) {
  std::list<std::unique_ptr<GemForgeSQCallback>> callbacks;
  if (!this->enableLSQ) {
    return callbacks;
  }
  // So far we only support LSQ for LLVMTraceCPU.
  assert(cpuDelegator->cpuType == GemForgeCPUDelegator::CPUTypeE::LLVM_TRACE &&
         "LSQ only works for LLVMTraceCPU.");
  // Find the element to be stored.
  StreamElement *storeElement = nullptr;
  auto storeStream = this->getStream(inst->getTDG().stream_store().stream_id());
  for (auto element : this->userElementMap.at(inst->getSeqNum())) {
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
  auto seqNum = inst->getSeqNum();
  assert(this->userElementMap.count(seqNum) != 0);
  // Check my element.
  auto storeStream = this->getStream(inst->getTDG().stream_store().stream_id());
  for (auto element : this->userElementMap.at(seqNum)) {
    if (element == nullptr) {
      continue;
    }
    if (element->stream == storeStream) {
      // Found it.
      element->stored = true;
      // Mark the stored element value ready.
      // No one is going to use it.
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
  // So far we only support LSQ for LLVMTraceCPU.
  assert(cpuDelegator->cpuType == GemForgeCPUDelegator::CPUTypeE::LLVM_TRACE &&
         "LSQ only works for LLVMTraceCPU.");
}

void StreamEngine::cpuStoreTo(Addr vaddr, int size) {
  if (this->numInflyStreamConfigurations == 0) {
    return;
  }
  if (this->peb.isHit(vaddr, size)) {
    hack("CPU stores to (%#x, %d), hits in PEB.\n", vaddr, size);
    this->flushPEB();
  }
}

void StreamEngine::initializeStreams(
    const ::LLVM::TDG::StreamRegion &streamRegion) {

  Stream::StreamArguments args;
  args.cpu = cpu;
  args.cpuDelegator = cpuDelegator;
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
  // Coalesced streams.
  std::unordered_map<uint64_t, uint64_t> coalescedGroupToStreamIdMap;
  for (const auto &streamInfo : streamRegion.streams()) {
    const auto &streamId = streamInfo.id();
    assert(this->streamMap.count(streamId) == 0 &&
           "Stream is already initialized.");
    /**
     * I know this is confusing. But there are two possible interpretation
     * of coalesce group. In both case, 0 is used as the invalid coalesce
     * group.
     * 1. In the old trace based implementation, this is some arbitrarily
     *    allocated number. The offset should be -1.
     * 2. In the static transform implementation, this is the base stream
     *    id, and the offset should be >= 0.
     */
    const auto &coalesceInfo = streamInfo.coalesce_info();
    auto coalesceGroup = coalesceInfo.base_stream();
    constexpr uint64_t InvalidCoalesceGroup = 0;

    // Set per stream field in stream args.
    args.staticId = streamId;
    args.name = streamInfo.name().c_str();

    if (coalesceGroup != InvalidCoalesceGroup && this->enableCoalesce) {

      auto staticCoalesced = coalesceInfo.offset() != -1;

      // First check if we have created the coalesced stream for the group.
      if (coalescedGroupToStreamIdMap.count(coalesceGroup) == 0) {
        auto newCoalescedStream = new CoalescedStream(args, staticCoalesced);
        newCoalescedStream->addStreamInfo(streamInfo);
        createdStreams.push_back(newCoalescedStream);
        this->streamMap.emplace(streamId, newCoalescedStream);
        this->coalescedStreamIdMap.emplace(streamId, streamId);
        coalescedGroupToStreamIdMap.emplace(coalesceGroup, streamId);
      } else {
        // This is not the first time we encounter this coalesce group.
        // Add the config to the coalesced stream.
        auto coalescedStreamId = coalescedGroupToStreamIdMap.at(coalesceGroup);
        auto coalescedStream = dynamic_cast<CoalescedStream *>(
            this->streamMap.at(coalescedStreamId));
        coalescedStream->addStreamInfo(streamInfo);
        this->coalescedStreamIdMap.emplace(streamId, coalescedStreamId);
      }

      // panic("Disabled stream coalesce so far.");
    } else {
      // Single stream can be immediately constructed and inserted into the
      // map.
      auto newStream = new SingleStream(args, streamInfo);
      createdStreams.push_back(newStream);
      this->streamMap.emplace(streamId, newStream);
    }
  }

  /**
   * Remember to finalize the streams.
   */
  for (auto newStream : createdStreams) {
    newStream->finalize();
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
  if (this->numInflyStreamConfigurations > 0) {
    // We require next tick.
    this->manager->scheduleTickNextCycle();
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
      this->numRunAHeadLengthDist.sample(stream->getAllocSize());
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
  element->clearInflyMemAccesses();
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

std::list<Stream *> StreamEngine::getConfigStreamsInRegion(
    const LLVM::TDG::StreamRegion &streamRegion) {
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
  return configStreams;
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
              return SA->getAllocSize() < SB->getAllocSize();
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
          if (backBaseS->getAllocSize() < maxAllocSize) {
            // The back base stream is lagging behind.
            // Reduce the maxAllocSize.
            maxAllocSize = backBaseS->getAllocSize();
          }
        }
      }
    }

    const auto &stepStreams = this->getStepStreamList(stepStream);
    for (size_t targetSize = 1;
         targetSize <= maxAllocSize && this->hasFreeElement(); ++targetSize) {
      for (auto S : stepStreams) {
        if (!this->hasFreeElement()) {
          break;
        }
        if (!S->configured) {
          continue;
        }
        if (S->getAllocSize() >= targetSize) {
          continue;
        }
        if (S != stepStream) {
          if (S->getAllocSize() >= stepStream->getAllocSize()) {
            // It doesn't make sense to allocate ahead than the step root.
            continue;
          }
        }
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
      if (baseS->getAllocSize() <= S->getAllocSize()) {
        // The base stream has not allocate the element we want.
        allocated = false;
      }
    } else {
      // The other one must be a constant stream.
      assert(baseS->stepRootStream == nullptr &&
             "Should be a constant stream.");
      // It should always be allocated.
    }
    // hack("Check base element from stream %s for stream %s allocated
    // %d.\n",
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
  auto newElement = this->removeFreeElement();
  this->numElementsAllocated++;
  if (S->getStreamType() == "load") {
    this->numLoadElementsAllocated++;
  } else if (S->getStreamType() == "store") {
    this->numStoreElementsAllocated++;
  }

  S->allocateElement(newElement);
}

void StreamEngine::releaseElementStepped(Stream *S, bool doThrottle) {

  /**
   * This function performs a normal release, i.e. release a stepped
   * element.
   */

  auto releaseElement = S->releaseElementStepped();
  /**
   * How to handle short streams?
   * There is a pathological case when the streams are short, and
   * increasing the run ahead length beyond the stream length does not
   * make sense. We do not throttle if the element is within the run ahead
   * length.
   */
  if (doThrottle) {
    if (releaseElement->FIFOIdx.entryIdx > S->maxSize) {
      this->throttleStream(S, releaseElement);
    }
  }

  const bool used = releaseElement->isFirstUserDispatched();

  /**
   * Sanity check that all the user are done with this element.
   */
  if (this->elementUserMap.count(releaseElement) != 0) {
    assert(this->elementUserMap.at(releaseElement).empty() &&
           "Some unreleased user instruction.");
  }

  if (S->getStreamType() == "load") {
    this->numLoadElementsStepped++;
    /**
     * For a stepped load element, it should be removed from the PEB.
     */
    assert(!this->peb.contains(releaseElement) &&
           "Used load element still in PEB when released.");
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
      auto cacheBlockVAddr =
          releaseElement->cacheBlockBreakdownAccesses[i].cacheBlockVAddr;
      auto &cacheBlockInfo = this->cacheBlockRefMap.at(cacheBlockVAddr);
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
        this->cacheBlockRefMap.erase(cacheBlockVAddr);
      }
    }
  }

  this->addFreeElement(releaseElement);
}

bool StreamEngine::releaseElementUnstepped(Stream *S) {
  auto releaseElement = S->releaseElementUnstepped();
  if (releaseElement) {
    if (S->getStreamType() == "load" && !S->getFloatManual()) {
      if (releaseElement->isAddrReady) {
        // This should be in PEB.
        this->peb.removeElement(releaseElement);
      }
    }
    this->addFreeElement(releaseElement);
  }
  return releaseElement != nullptr;
}

void StreamEngine::stepElement(Stream *S) {
  auto element = S->stepElement();
  if (S->getStreamType() == "load" && !S->getFloatManual()) {
    if (!element->isFirstUserDispatched() && element->isAddrReady) {
      // This issued element is stepped but not used, remove from PEB.
      this->peb.removeElement(element);
    }
  }
}

void StreamEngine::unstepElement(Stream *S) {
  auto element = S->unstepElement();
  // We may need to add this back to PEB.
  if (S->getStreamType() == "load" && !S->getFloatManual()) {
    if (!element->isFirstUserDispatched() && element->isAddrReady) {
      this->peb.addElement(element);
    }
  }
}

std::vector<StreamElement *> StreamEngine::findReadyElements() {
  std::vector<StreamElement *> readyElements;

  auto areBaseElementsValReady = [](StreamElement *element) -> bool {
    bool ready = true;
    for (const auto &baseElement : element->baseElements) {
      if (baseElement->stream == nullptr) {
        // ! Some bug here that the base element is already released.
        continue;
      }
      if (element->stream->baseStreams.count(baseElement->stream) == 0 &&
          element->stream->backBaseStreams.count(baseElement->stream) == 0) {
        continue;
      }
      if (baseElement->FIFOIdx.entryIdx > element->FIFOIdx.entryIdx) {
        // ! Some bug here that the base element is already used by others.
        // TODO: Better handle all these.
        continue;
      }
      if (!baseElement->isValueReady) {
        ready = false;
        break;
      }
    }
    return ready;
  };

  /**
   * We iterate through all configured streams' elements.
   * In this way, elements are marked ready in order, i.e. if
   * one element is not ready then we break searching in this stream.
   */
  for (const auto &idStream : this->streamMap) {
    auto S = idStream.second;
    if (!S->configured) {
      continue;
    }
    for (auto &dynS : S->dynamicStreams) {
      if (!dynS.configExecuted) {
        // The StreamConfig has not been executed, do not issue.
        continue;
      }
      for (auto element = dynS.tail->next; element != nullptr;
           element = element->next) {
        assert(element->stream == S && "Sanity check that streams match.");
        if (element->isAddrReady) {
          // Already ready.
          continue;
        }
        /**
         * To avoid overhead, if an element is aliased, we do not try to
         * issue it until the first user is dispatched.
         */
        if (element->isAddrAliased && !element->isFirstUserDispatched()) {
          break;
        }
        auto baseElementsValReady = areBaseElementsValReady(element);
        if (baseElementsValReady) {
          S_ELEMENT_DPRINTF(element, "Found ready.\n");
          readyElements.emplace_back(element);
        }
      }
    }
  }

  // /**
  //  * Old implementation to search for ready elements.
  //  */
  // for (auto &element : this->FIFOArray) {
  //   if (element.stream == nullptr) {
  //     // Not allocated, ignore.
  //     continue;
  //   }
  //   if (element.isAddrReady) {
  //     // We already issued request for this element.
  //     continue;
  //   }
  //   // Check if StreamConfig is already executed.
  //   if (!element.stream->isStreamConfigureExecuted(
  //           element.FIFOIdx.configSeqNum)) {
  //     // This stream is not fully configured yet.
  //     continue;
  //   }
  //   // Check if all the base element are value ready.
  //   bool ready = areBaseElementsValReady(&element);
  //   if (ready) {
  //     S_ELEMENT_DPRINTF(&element, "Found ready.\n");
  //     readyElements.emplace_back(&element);
  //   }
  // }
  return readyElements;
}

void StreamEngine::issueElements() {
  // Find all ready elements.
  auto readyElements = this->findReadyElements();

  /**
   * Sort the ready elements by create cycle and relative order within
   * the single stream.
   */
  // Sort the ready elements, by their create cycle.
  std::sort(readyElements.begin(), readyElements.end(),
            [](const StreamElement *A, const StreamElement *B) -> bool {
              if (A->allocateCycle != B->allocateCycle) {
                return A->allocateCycle < B->allocateCycle;
              }
              if (A->stream != B->stream) {
                // Break the time by stream address.
                return reinterpret_cast<uint64_t>(A->stream) <
                       reinterpret_cast<uint64_t>(B->stream);
              }
              const auto &AIdx = A->FIFOIdx;
              const auto &BIdx = B->FIFOIdx;
              return BIdx > AIdx;
            });
  for (auto &element : readyElements) {
    element->markAddrReady(cpuDelegator);

    if (element->stream->isMemStream()) {
      // Increase the reference of the cache block if we enable merging.
      if (this->enableMerge) {
        for (int i = 0; i < element->cacheBlocks; ++i) {
          auto cacheBlockAddr =
              element->cacheBlockBreakdownAccesses[i].cacheBlockVAddr;
          this->cacheBlockRefMap
              .emplace(std::piecewise_construct,
                       std::forward_as_tuple(cacheBlockAddr),
                       std::forward_as_tuple())
              .first->second.reference++;
        }
      }
      // Issue the element.
      this->issueElement(element);
    } else {
      /**
       * This is an IV stream. We assume their size be less than 8 bytes
       * and copy the address directly as the value.
       * TODO: This is not enough to support other type of IV stream, like
       * TODO: the back dependence of pointer chasing stream.
       */
      assert(element->size <= 8 && "IV Stream size greater than 8 bytes.");
      element->setValue(element->addr, element->size,
                        reinterpret_cast<uint8_t *>(&element->addr));
    }
  }
}
void StreamEngine::fetchedCacheBlock(Addr cacheBlockVAddr,
                                     StreamMemAccess *memAccess) {
  // Check if we still have the cache block.
  if (!this->enableMerge) {
    return;
  }
  if (this->cacheBlockRefMap.count(cacheBlockVAddr) == 0) {
    return;
  }
  auto &cacheBlockInfo = this->cacheBlockRefMap.at(cacheBlockVAddr);
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

  S_ELEMENT_DPRINTF(element, "Issue.\n");

  auto S = element->stream;
  if (S->getStreamType() == "load") {
    if (element->flushed) {
      // S_ELEMENT_HACK(element, "Reissue element.\n");
    }
    this->numLoadElementsFetched++;
    S->statistic.numFetched++;
    // Add to the PEB if the first user has not been dispatched.
    if (!S->getFloatManual() && !element->isFirstUserDispatched() &&
        !element->isStepped) {
      this->peb.addElement(element);
    }
  }

  /**
   * A quick hack to coalesce continuous elements that completely overlap.
   */
  this->coalesceContinuousDirectMemStreamElement(element);

  for (size_t i = 0; i < element->cacheBlocks; ++i) {
    auto &cacheBlockBreakdown = element->cacheBlockBreakdownAccesses[i];

    // Normal case: really fetching this from the cache,
    // i.e. not merged & not handled by placement manager.
    // ! Always fetch the whole cache line, this is an
    // ! optimization for continuous load stream.
    // TODO: Continuous load stream should really be allocated in
    // TODO: granularity of cache lines (not stream elements).

    // Check if this cache line is already done.
    if (cacheBlockBreakdown.state !=
        CacheBlockBreakdownAccess::StateE::Initialized) {
      continue;
    }

    auto vaddr = cacheBlockBreakdown.cacheBlockVAddr;
    auto packetSize = cpuDelegator->cacheLineSize();
    Addr paddr;
    if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
      S_ELEMENT_DPRINTF(element, "Fault on vaddr %#x,\n", vaddr);
      cacheBlockBreakdown.state = CacheBlockBreakdownAccess::StateE::Faulted;
      /**
       * The current mechanism to mark value ready is too hacky.
       * We rely on the setValue() to call tryMarkValueReady().
       * However, since Faulted is also considered ready, we have to
       * call tryMarkValueReady() whenver we set a block to Faulted state.
       * TODO: Improve this poor design.
       */
      element->tryMarkValueReady();
      continue;
    }

    // Allocate the book-keeping StreamMemAccess.
    auto memAccess = element->allocateStreamMemAccess(cacheBlockBreakdown);
    auto pkt = GemForgePacketHandler::createGemForgePacket(
        paddr, packetSize, memAccess, nullptr, cpuDelegator->dataMasterId(), 0,
        0);
    S_ELEMENT_DPRINTF(element, "Issued %d request to %#x %d.\n", i, vaddr,
                      packetSize);
    S->statistic.numIssuedRequest++;
    cpuDelegator->sendRequest(pkt);

    // Mark the state.
    cacheBlockBreakdown.state = CacheBlockBreakdownAccess::StateE::Issued;
    cacheBlockBreakdown.memAccess = memAccess;
    memAccess->registerReceiver(element);
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

  S_ELEMENT_DPRINTF(element, "Writeback.\n");

  // hack("Send packt for stream %s.\n", S->getStreamName().c_str());

  for (size_t i = 0; i < element->cacheBlocks; ++i) {
    auto &cacheBlockBreakdown = element->cacheBlockBreakdownAccesses[i];

    // Translate the virtual address.
    auto vaddr = cacheBlockBreakdown.virtualAddr;
    auto packetSize = cacheBlockBreakdown.size;
    Addr paddr;
    if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
      panic("Failed translate vaddr %#x.\n", vaddr);
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
    auto pkt = GemForgePacketHandler::createGemForgePacket(
        paddr, packetSize, memAccess, this->writebackCacheLine,
        cpuDelegator->dataMasterId(), 0, 0);
    cpuDelegator->sendRequest(pkt);
  }
}

void StreamEngine::dumpFIFO() const {
  inform("Total elements %d, free %d, totalRunAhead %d\n",
         this->FIFOArray.size(), this->numFreeFIFOEntries,
         this->getTotalRunAheadLength());

  for (const auto &IdStream : this->streamMap) {
    auto S = IdStream.second;
    if (S->configured) {
      S->dump();
    }
  }
}

void StreamEngine::dumpUser() const {
  for (const auto &userElement : this->userElementMap) {
    auto user = userElement.first;
    inform("--seqNum %llu used element.\n", user);
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
      "stream.stats." + std::to_string(cpuDelegator->cpuId()) + ".txt";
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
    // We have reached the threshold to allow the stream to run further
    // ahead.
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

  auto fullPath = cpuDelegator->getTraceExtraFolder() + "/" + relativePath;
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
 * that can coexist with the current stream, and assign InitMaxSize number
 *of elements to these streams, which is called BasicEntries.
 * * BasicEntries = TotalAliveStreams * InitMaxSize.
 *
 * Then we want to know how many of these BasicEntries is already assigned
 *to streams. This number is called AssignedBasicEntries.
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
    inform("AssignedEntries %d UnAssignedEntries %d BasicEntries %d "
           "AssignedBasicEntries %d AvailableEntries %d UpperBoundEntries "
           "%d.\n",
           assignedEntries, unAssignedEntries, basicEntries,
           assignedBasicEntries, availableEntries, upperBoundEntries);
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

bool StreamEngine::GemForgeStreamEngineLQCallback::getAddrSize(
    Addr &addr, uint32_t &size) const {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  // Check if the address is ready.
  if (!this->element->isAddrReady) {
    return false;
  }
  addr = this->element->addr;
  size = this->element->size;
  return true;
}

bool StreamEngine::GemForgeStreamEngineLQCallback::isIssued() const {
  /**
   * So far the element is considered issued when its address is ready.
   */
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  return this->element->isAddrReady;
}

bool StreamEngine::GemForgeStreamEngineLQCallback::isValueLoaded() {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");

  /**
   * We can directly check for element->isValueReady, but instead we
   * call areUsedStreamReady() so that StreamEngine can mark the
   * firstCheckCycle for the element, hence it can throttle the stream.
   */
  return this->element->se->areUsedStreamsReady(this->args);
}

void StreamEngine::GemForgeStreamEngineLQCallback::RAWMisspeculate() {
  assert(this->FIFOIdx == this->element->FIFOIdx &&
         "Element already released.");
  /**
   * Disable this for now.
   */
  // cpu->getIEWStage().misspeculateInst(userInst);
  this->element->se->RAWMisspeculate(this->element);
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

void StreamEngine::coalesceContinuousDirectMemStreamElement(
    StreamElement *element) {

  /**
   * ! Disable this feature for now cause the previous element
   * ! may be stepped without value ready for conditionally
   * ! used streams.
   */
  const bool enableCoalesceContinuousElement = true;
  if (!enableCoalesceContinuousElement) {
    return;
  }

  // Check if this is the first element.
  if (element->FIFOIdx.entryIdx == 0) {
    return;
  }
  // Check if this element is flushed.
  if (element->flushed) {
    return;
  }
  auto S = element->stream;
  if (!S->isDirectMemStream()) {
    return;
  }
  // Get the previous element.
  auto prevElement = S->getPrevElement(element);
  // We found the previous element. Check if completely overlap.
  assert(prevElement->FIFOIdx.entryIdx + 1 == element->FIFOIdx.entryIdx &&
         "Mismatch entryIdx for prevElement.");
  assert(prevElement->FIFOIdx.streamId == element->FIFOIdx.streamId &&
         "Mismatch streamId for prevElement.");

  // Check if the previous element has the cache line.
  if (!prevElement->isCacheBlockedValue) {
    return;
  }
  assert(prevElement->cacheBlocks && "No block in prevElement.");

  auto &prevElementMinBlockVAddr =
      prevElement->cacheBlockBreakdownAccesses[0].cacheBlockVAddr;
  for (int cacheBlockIdx = 0; cacheBlockIdx < element->cacheBlocks;
       ++cacheBlockIdx) {
    auto &block = element->cacheBlockBreakdownAccesses[cacheBlockIdx];
    assert(block.state == CacheBlockBreakdownAccess::StateE::Initialized);
    if (block.cacheBlockVAddr < prevElementMinBlockVAddr) {
      // Underflow.
      continue;
    }
    auto blockOffset = (block.cacheBlockVAddr - prevElementMinBlockVAddr) /
                       element->cacheBlockSize;
    if (blockOffset >= prevElement->cacheBlocks) {
      // Overflow.
      continue;
    }
    // Found a match.
    // Check if the previous block faulted.
    const auto &prevBlock =
        prevElement->cacheBlockBreakdownAccesses[blockOffset];
    if (prevBlock.state == CacheBlockBreakdownAccess::StateE::Faulted) {
      // Also mark this block faulted.
      block.state = CacheBlockBreakdownAccess::StateE::Faulted;
      element->tryMarkValueReady();
    } else if (prevBlock.state == CacheBlockBreakdownAccess::StateE::Ready) {
      // We can copy the value.
      auto offset = prevElement->mapVAddrToValueOffset(block.cacheBlockVAddr,
                                                       element->cacheBlockSize);
      element->setValue(block.cacheBlockVAddr, element->cacheBlockSize,
                        &prevElement->value.at(offset));
    } else if (prevBlock.state == CacheBlockBreakdownAccess::StateE::Issued) {
      // Register myself as a receiver.
      assert(prevBlock.memAccess &&
             "Missing memAccess for Issued cache block.");
      block.memAccess = prevBlock.memAccess;
      block.memAccess->registerReceiver(element);
      block.state = CacheBlockBreakdownAccess::StateE::Issued;
    }
  }
}

void StreamEngine::flushPEB() {
  for (auto element : this->peb.elements) {
    assert(element->isAddrReady);
    assert(!element->isStepped);
    assert(!element->isFirstUserDispatched());

    // Clear the element to just allocate state.
    element->isAddrReady = false;
    element->isValueReady = false;

    // Raise the flush flag.
    element->flushed = true;

    element->valueReadyCycle = Cycles(0);
    element->firstCheckCycle = Cycles(0);

    element->addr = 0;
    element->size = 0;
    element->clearInflyMemAccesses();
    element->clearCacheBlocks();
    std::fill(element->value.begin(), element->value.end(), 0);
  }
  this->peb.elements.clear();
}

void StreamEngine::RAWMisspeculate(StreamElement *element) {
  assert(!this->peb.contains(element) && "RAWMisspeculate on PEB element.");
  // Still, we flush the PEB when LQ misspeculate happens.
  this->flushPEB();

  // Revert this element to just allocate state.
  element->flushed = true;
  element->isAddrReady = false;
  element->isValueReady = false;
  element->valueReadyCycle = Cycles(0);
  element->firstCheckCycle = Cycles(0);

  element->addr = 0;
  element->size = 0;
  element->clearInflyMemAccesses();
  element->clearCacheBlocks();
  std::fill(element->value.begin(), element->value.end(), 0);
}

StreamEngine *StreamEngineParams::create() { return new StreamEngine(this); }
