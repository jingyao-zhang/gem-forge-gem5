#include "stream_engine.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"

#include "base/misc.hh"
#include "base/trace.hh"
#include "debug/StreamEngine.hh"

namespace {
static std::string DEBUG_STREAM_NAME =
    "(IV solve_l2r_l1l2_svc bb152 bb152::tmp154(phi))";

bool isDebugStream(Stream *S) {
  return S->getStreamName() == DEBUG_STREAM_NAME;
}

void debugStream(Stream *S, const char *message) {
  inform("%20s: Stream %50s config %1d step %3d allocated %3d max %3d.\n",
         message, S->getStreamName().c_str(), S->configured, S->stepSize,
         S->allocSize, S->maxSize);
}

} // namespace

StreamEngine::StreamEngine()
    : TDGAccelerator(), streamPlacementManager(nullptr), isOracle(false) {}

StreamEngine::~StreamEngine() {
  if (this->streamPlacementManager != nullptr) {
    delete this->streamPlacementManager;
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
}

void StreamEngine::handshake(LLVMTraceCPU *_cpu,
                             TDGAcceleratorManager *_manager) {
  TDGAccelerator::handshake(_cpu, _manager);

  auto cpuParams = dynamic_cast<const LLVMTraceCPUParams *>(_cpu->params());
  this->setIsOracle(cpuParams->streamEngineIsOracle);
  this->maxRunAHeadLength = cpuParams->streamEngineMaxRunAHeadLength;
  this->currentTotalRunAheadLength = 0;
  // this->maxTotalRunAheadLength =
  // cpuParams->streamEngineMaxTotalRunAHeadLength;
  this->maxTotalRunAheadLength = this->maxRunAHeadLength * 512;
  if (cpuParams->streamEngineThrottling == "static") {
    this->throttling = ThrottlingE::STATIC;
  } else {
    this->throttling = ThrottlingE::DYNAMIC;
  }
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

  this->initializeFIFO(this->maxTotalRunAheadLength);

  if (this->enableStreamPlacement) {
    this->streamPlacementManager = new StreamPlacementManager(cpu, this);
  }
}

void StreamEngine::regStats() {
  this->numConfigured.name(this->manager->name() + ".stream.numConfigured")
      .desc("Number of streams configured.")
      .prereq(this->numConfigured);
  this->numStepped.name(this->manager->name() + ".stream.numStepped")
      .desc("Number of streams stepped.")
      .prereq(this->numStepped);
  this->numStreamMemRequests
      .name(this->manager->name() + ".stream.numStreamMemRequests")
      .desc("Number of stream memory requests.")
      .prereq(this->numStreamMemRequests);
  this->numElements.name(this->manager->name() + ".stream.numElements")
      .desc("Number of stream elements created.")
      .prereq(this->numElements);
  this->numElementsUsed.name(this->manager->name() + ".stream.numElementsUsed")
      .desc("Number of stream elements used.")
      .prereq(this->numElementsUsed);
  this->numUnconfiguredStreamUse
      .name(this->manager->name() + ".stream.numUnconfiguredStreamUse")
      .desc("Number of unconfigured stream use request.")
      .prereq(this->numUnconfiguredStreamUse);
  this->numConfiguredStreamUse
      .name(this->manager->name() + ".stream.numConfiguredStreamUse")
      .desc("Number of Configured stream use request.")
      .prereq(this->numConfiguredStreamUse);
  this->entryWaitCycles.name(this->manager->name() + ".stream.entryWaitCycles")
      .desc("Number of cycles from first checked ifReady to ready.")
      .prereq(this->entryWaitCycles);
  this->numMemElements.name(this->manager->name() + ".stream.numMemElements")
      .desc("Number of mem stream elements created.")
      .prereq(this->numMemElements);
  this->numMemElementsFetched
      .name(this->manager->name() + ".stream.numMemElementsFetched")
      .desc("Number of mem stream elements fetched from cache.")
      .prereq(this->numMemElementsFetched);
  this->numMemElementsUsed
      .name(this->manager->name() + ".stream.numMemElementsUsed")
      .desc("Number of mem stream elements used.")
      .prereq(this->numMemElementsUsed);
  this->memEntryWaitCycles
      .name(this->manager->name() + ".stream.memEntryWaitCycles")
      .desc("Number of cycles of a mem entry from first checked ifReady to "
            "ready.")
      .prereq(this->memEntryWaitCycles);

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

  this->numAccessPlacedInCacheLevel.init(0, 5, 1)
      .name(this->manager->name() + ".stream.numAccessPlacedInCacheLevel")
      .desc("Number of accesses placed in different cache level.")
      .flags(Stats::pdf);
  this->numAccessHitHigherThanPlacedCacheLevel.init(0, 5, 1)
      .name(this->manager->name() +
            ".stream.numAccessHitHigherThanPlacedCacheLevel")
      .desc("Number of accesses hit in higher level than placed cache.")
      .flags(Stats::pdf);
  this->numAccessHitLowerThanPlacedCacheLevel.init(0, 5, 1)
      .name(this->manager->name() +
            ".stream.numAccessHitLowerThanPlacedCacheLevel")
      .desc("Number of accesses hit in lower level than placed cache.")
      .flags(Stats::pdf);

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
  this->numCacheLevel.name(this->manager->name() + ".stream.numCacheLevel")
      .desc("Number of cache levels")
      .prereq(this->numCacheLevel);
}

bool StreamEngine::canStreamConfig(const StreamConfigInst *inst) const {
  /**
   * A stream can be configured iff. we can guarantee that it will be allocate
   * one entry when configured.
   *
   * If this this the first time we encounter the stream, we check the number of
   * free entries. Otherwise, we ALSO ensure that allocSize < maxSize.
   */

  if (this->numFreeFIFOEntries <
      inst->getTDG().stream_config().configs_size()) {
    // Not enough free entries for each stream.
    return false;
  }
  const auto &configs = inst->getTDG().stream_config().configs();

  for (const auto &config : configs) {
    auto stream_id = config.stream_id();
    auto iter = this->streamMap.find(stream_id);
    if (iter != this->streamMap.end()) {
      // Check if we have quota for this stream.
      auto S = iter->second;
      if (S->allocSize == S->maxSize) {
        // No more quota.
        return false;
      }
    }
  }
  return true;
}

void StreamEngine::dispatchStreamConfigure(StreamConfigInst *inst) {
  assert(this->canStreamConfig(inst) && "Cannot configure stream.");

  this->numConfigured++;

  const auto &configs = inst->getTDG().stream_config().configs();
  std::list<Stream *> configStreams;
  for (const auto &config : configs) {
    auto S = this->getOrInitializeStream(config);
    assert(!S->configured && "The stream should not be configured.");
    S->configured = true;

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

    configStreams.push_back(S);
  }

  // 3. Allocate new entries one by one for all streams.
  // The first element is guaranteed to be allocated.
  for (auto S : configStreams) {
    assert(this->hasFreeElement());
    assert(S->allocSize < S->maxSize);
    assert(this->areBaseElementAllocated(S));
    this->allocateElement(S);
  }
  // Allocate the remaining free entries.
  while (this->hasFreeElement()) {
    bool allocated = false;
    for (auto S : configStreams) {
      if (S->allocSize == S->maxSize) {
        // This stream has already reached the run ahead limit.
        continue;
      }
      if (!this->areBaseElementAllocated(S)) {
        // The base element is not yet allocated.
        continue;
      }
      this->allocateElement(S);
      allocated = true;
    }
    if (!allocated) {
      // No more streams can be allocate more entries.
      break;
    }
  }
  for (auto S : configStreams) {
    if (isDebugStream(S)) {
      debugStream(S, "Dispatch Config");
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
  if (isDebugStream(stepStream)) {
    debugStream(stepStream, "Dispatch Step");
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
     * totalRunAheadLength, which only considers configured streams. Therefore,
     * we can not throttle for the last element (streamEnd), as some base
     * streams may already be cleared, and we get an inaccurate
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

  /**
   * Try to allocate more elements.
   * Set a target, try to make sure all streams reach this target.
   * Then increment the target.
   */
  for (size_t targetSize = 1;
       targetSize <= stepStream->maxSize && this->hasFreeElement();
       ++targetSize) {
    for (auto S : stepStreams) {
      if (!this->hasFreeElement()) {
        break;
      }
      if (!S->configured) {
        continue;
      }
      if (S->allocSize >= targetSize) {
        continue;
      }
      if (S->allocSize > stepStream->allocSize) {
        // It doesn't make sense to allocate before the step root.
        continue;
      }
      this->allocateElement(S);
    }
  }
  if (isDebugStream(stepStream)) {
    debugStream(stepStream, "Commit Step");
  }
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
     * In such case we assume it's ready and use a nullptr as a special element
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

      elementSet.insert(S->stepped->next);
    }
  }
}

bool StreamEngine::areUsedStreamsReady(const LLVMDynamicInst *inst) {
  assert(this->userElementMap.count(inst) != 0);

  bool ready = true;
  for (auto &element : this->userElementMap.at(inst)) {
    if (element == nullptr) {
      /**
       * Sometimes thiere is use after stream end,
       * in such case we assume the element is copied to register and
       * is ready.
       */
      continue;
    }
    // Mark the first check cycle.
    if (element->firstCheckCycle == 0) {
      element->firstCheckCycle = cpu->curCycle();
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
  // Simply release the entry.
  this->userElementMap.erase(inst);
}

void StreamEngine::dispatchStreamEnd(StreamEndInst *inst) {

  const auto &endStreamIds = inst->getTDG().stream_end().stream_ids();

  for (auto iter = endStreamIds.rbegin(), end = endStreamIds.rend();
       iter != end; ++iter) {
    // Release in reverse order.
    auto streamId = *iter;
    auto S = this->getStream(streamId);

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

  for (auto iter = endStreamIds.rbegin(), end = endStreamIds.rend();
       iter != end; ++iter) {
    // Release in reverse order.
    auto streamId = *iter;
    auto S = this->getStream(streamId);
    /**
     * Release the last element we stepped at dispatch.
     */
    this->releaseElement(S);
    if (isDebugStream(S)) {
      debugStream(S, "Commit End");
    }
  }
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
      break;
    }
  }
}

void StreamEngine::commitStreamStore(StreamStoreInst *inst) {}

bool StreamEngine::handle(LLVMDynamicInst *inst) { return false; }

CoalescedStream *
StreamEngine::getOrInitializeCoalescedStream(uint64_t stepRootStreamId,
                                             int32_t coalesceGroup) {
  auto stepRootIter = this->coalescedStreamMap.find(stepRootStreamId);
  if (stepRootIter == this->coalescedStreamMap.end()) {
    stepRootIter = this->coalescedStreamMap
                       .emplace(std::piecewise_construct,
                                std::forward_as_tuple(stepRootStreamId),
                                std::forward_as_tuple())
                       .first;
  }

  auto coalesceGroupIter = stepRootIter->second.find(coalesceGroup);
  if (coalesceGroupIter == stepRootIter->second.end()) {
    coalesceGroupIter =
        stepRootIter->second
            .emplace(std::piecewise_construct,
                     std::forward_as_tuple(coalesceGroup),
                     std::forward_as_tuple(cpu, this, this->isOracle,
                                           this->maxRunAHeadLength, "static"))
            .first;
  }

  return &(coalesceGroupIter->second);
}

Stream *StreamEngine::getOrInitializeStream(
    const LLVM::TDG::TDGInstruction_StreamConfigExtra_SingleConfig &config) {
  const auto &streamId = config.stream_id();
  auto iter = this->streamMap.find(streamId);
  if (iter == this->streamMap.end()) {
    /**
     * The configInst does not contain much information,
     * we need to load the info protobuf file.
     * Luckily, this would only happen once for every stream.
     */
    auto infoPath = cpu->getTraceExtraFolder() + "/" + config.info_path();
    auto streamInfo = StreamEngine::parseStreamInfoFromFile(infoPath);
    auto coalesceGroup = streamInfo.coalesce_group();

    Stream *newStream = nullptr;
    if (coalesceGroup != -1 && this->enableCoalesce) {
      // // We should handle it as a coalesced stream.

      // // Get the step root stream id.
      // uint64_t stepRootStreamId = streamInfo.id();
      // if (streamInfo.chosen_base_step_root_ids_size() > 1) {
      //   panic("More than one step root stream for coalesced streams.");
      // } else if (streamInfo.chosen_base_step_root_ids_size() == 1) {
      //   // We have one step root stream.
      //   stepRootStreamId = streamInfo.chosen_base_step_root_ids(0);
      // }

      // auto coalescedStream =
      //     getOrInitializeCoalescedStream(stepRootStreamId, coalesceGroup);

      // // Add the logical stream to the coalesced stream.
      // coalescedStream->addLogicalStreamIfNecessary(configInst);

      // newStream = coalescedStream;
      panic("Disabled stream coalesce so far.");

    } else {
      newStream = new SingleStream(config, cpu, this, this->isOracle,
                                   this->maxRunAHeadLength, "static");
    }

    iter =
        this->streamMap
            .emplace(std::piecewise_construct, std::forward_as_tuple(streamId),
                     std::forward_as_tuple(newStream))
            .first;
  }
  return iter->second;
}

Stream *StreamEngine::getStream(uint64_t streamId) const {
  auto iter = this->streamMap.find(streamId);
  if (iter == this->streamMap.end()) {
    panic("Failed to find stream %lu.\n", streamId);
  }
  return iter->second;
}

void StreamEngine::tick() {
  this->issueElements();
  if (curTick() % 10000 == 0) {
    this->updateAliveStatistics();
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
      this->numRunAHeadLengthDist.sample(stream->getRunAheadLength());
    }
    if (!stream->isConfigured()) {
      continue;
    }
    if (stream->isMemStream()) {
      totalAliveElements += stream->getAliveElements();
      totalAliveMemStreams++;
      for (const auto &cacheBlockAddrPair : stream->getAliveCacheBlocks()) {
        totalAliveCacheBlocks.insert(cacheBlockAddrPair.first);
      }
    }
  }
  this->numTotalAliveElements.sample(totalAliveElements);
  this->numTotalAliveCacheBlocks.sample(totalAliveCacheBlocks.size());
  this->numTotalAliveMemStreams.sample(totalAliveMemStreams);
}

LLVM::TDG::StreamInfo
StreamEngine::parseStreamInfoFromFile(const std::string &infoPath) {
  ProtoInputStream infoIStream(infoPath);
  LLVM::TDG::StreamInfo info;
  if (!infoIStream.read(info)) {
    panic("Failed to read in the stream info from file %s.", infoPath.c_str());
  }
  return info;
}

void StreamEngine::initializeFIFO(size_t totalElements) {
  panic_if(!this->FIFOArray.empty(), "FIFOArray has already been initialized.");

  this->FIFOArray.reserve(totalElements);
  while (this->FIFOArray.size() < totalElements) {
    this->FIFOArray.emplace_back();
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
          // Cycle dependence found.
          if (stackStatusMap.at(depS) == 1) {
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

bool StreamEngine::areBaseElementAllocated(Stream *S) {
  // Find the base element.
  for (auto baseS : S->baseStreams) {
    if (baseS->getLoopLevel() != S->getLoopLevel()) {
      continue;
    }

    if (baseS->stepRootStream == S->stepRootStream) {
      if (baseS->allocSize - baseS->stepSize <= S->allocSize - S->stepSize) {
        // The base stream has not allocate the element we want.
        return false;
      }
    } else {
      // The other one must be a constant stream.
      assert(baseS->stepRootStream == nullptr &&
             "Should be a constant stream.");
      if (baseS->stepped->next == nullptr) {
        return false;
      }
    }
  }
  return true;
}

void StreamEngine::allocateElement(Stream *S) {
  assert(this->hasFreeElement());
  assert(S->configured && "Stream should be configured to allocate element.");
  auto newElement = this->removeFreeElement();

  S->FIFOIdx.next();
  newElement->stream = S;
  newElement->FIFOIdx = S->FIFOIdx;

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

  newElement->allocateCycle = cpu->curCycle();

  // Create all the cache line this element will touch.
  if (S->isMemStream()) {
    S->prepareNewElement(newElement);
    const int cacheBlockSize = cpu->system->cacheLineSize();
    auto lhsCacheBlock = newElement->addr & (~(cacheBlockSize - 1));
    auto rhsCacheBlock =
        (newElement->addr + newElement->size - 1) & (~(cacheBlockSize - 1));
    while (lhsCacheBlock <= rhsCacheBlock) {
      if (newElement->cacheBlocks >= StreamElement::MAX_CACHE_BLOCKS) {
        panic("More than %d cache blocks for one stream element, address %lu "
              "size %lu.",
              newElement->cacheBlocks, newElement->addr, newElement->size);
      }
      newElement->cacheBlockAddrs[newElement->cacheBlocks] = lhsCacheBlock;
      newElement->cacheBlocks++;
      if (lhsCacheBlock == 0xFFFFFFFFFFFFFFC0) {
        // This is the last block in the address space.
        // Something wrong here.
        break;
      }
      lhsCacheBlock += cacheBlockSize;
    }
  } else {
    // IV stream already ready.
    newElement->isAddrReady = true;
    newElement->isValueReady = true;
    newElement->valueReadyCycle = cpu->curCycle();
  }

  // Append to the list.
  S->head->next = newElement;
  S->allocSize++;
  S->head = newElement;
}

void StreamEngine::releaseElement(Stream *S) {
  assert(S->stepSize > 0 && "No element to release.");
  auto releaseElement = S->tail->next;

  // If the element is stored, we reissue the store request.
  if (releaseElement->stored) {
    this->issueElement(releaseElement);
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
    // Check if all the base element are value ready.
    bool ready = true;
    for (const auto &baseElement : element.baseElements) {
      if (!baseElement->isValueReady) {
        ready = false;
        break;
      }
    }
    if (ready) {
      readyElements.emplace_back(&element);
    }
  }

  // Sort the ready elements, by their create cycle.
  std::sort(readyElements.begin(), readyElements.end(),
            [](const StreamElement *A, const StreamElement *B) -> bool {
              return B->allocateCycle > A->allocateCycle;
            });
  for (auto &element : readyElements) {
    element->isAddrReady = true;
    this->issueElement(element);
  }
}

void StreamEngine::issueElement(StreamElement *element) {
  assert(element->isAddrReady && "Address should be ready.");

  assert(element->stream->isMemStream() &&
         "Should never issue element for IVStream.");

  auto S = element->stream;
  // hack("Send packt for stream %s.\n", S->getStreamName().c_str());

  for (size_t i = 0; i < element->cacheBlocks; ++i) {
    const auto cacheBlockAddr = element->cacheBlockAddrs[i];
    Addr paddr;
    if (cpu->isStandalone()) {
      paddr = cpu->translateAndAllocatePhysMem(cacheBlockAddr);
    } else {
      panic("Stream so far can only work in standalone mode.");
    }

    // Bring in the whole cache block.
    auto packetSize = cpu->system->cacheLineSize();

    auto pkt = cpu->sendRequest(paddr, packetSize, element, nullptr,
                                reinterpret_cast<Addr>(element));

    if (S->getStreamType() == "load") {
      element->inflyLoadPackets.insert(pkt);
    } else if (S->getStreamType() == "store") {
      // Store can be directly value ready.
      element->isValueReady = true;
      element->valueReadyCycle = cpu->curCycle();
    }
  }
}

void StreamEngine::dumpFIFO() const {
  inform("Total elements %d, free %d, totalRunAhead %d\n",
         this->FIFOArray.size(), this->numFreeFIFOEntries,
         this->getTotalRunAheadLength());

  for (const auto &IdStream : this->streamMap) {
    auto S = IdStream.second;
    debugStream(S, "");
  }
}

void StreamEngine::dump() {
  if (this->streamPlacementManager != nullptr) {
    this->streamPlacementManager->dumpCacheStreamAwarePortStatus();
  }
  this->dumpFIFO();
}

void StreamEngine::throttleStream(Stream *S, StreamElement *element) {
  if (this->throttling == ThrottlingE::STATIC) {
    // Static means no throttling.
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
      // All streams with the same stepRootStream must have the same run ahead
      // length.
      const auto &streamList = this->getStepStreamList(stepRootStream);
      auto totalRunAheadLength = this->getTotalRunAheadLength();
      // Only increase the run ahead length if the totalRunAheadLength is within
      // the 90% of the total FIFO entries. Need better solution here.
      const auto incrementStep = 2;
      // auto newTotalRunAheadLength =
      //     totalRunAheadLength + streamList.size() * incrementStep;
      if (static_cast<float>(totalRunAheadLength) <
          0.9f * static_cast<float>(this->FIFOArray.size())) {
        for (auto stepS : streamList) {
          // Increase the run ahead length by 2.
          stepS->maxSize += incrementStep;
        }
        assert(S->maxSize == oldRunAheadSize + 2 &&
               "RunAheadLength is not increased.");
      }
      // No matter what, clear the lateFetchCount for all streams within the
      // step group.
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