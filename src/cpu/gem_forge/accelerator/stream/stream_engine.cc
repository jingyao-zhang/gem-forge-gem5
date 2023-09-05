#include "stream_engine.hh"
#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"
#include "stream_compute_engine.hh"
#include "stream_data_traffic_accumulator.hh"
#include "stream_float_controller.hh"
#include "stream_lsq_callback.hh"
#include "stream_ndc_controller.hh"
#include "stream_range_sync_controller.hh"
#include "stream_region_controller.hh"
#include "stream_throttler.hh"

#include "base/trace.hh"
#include "debug/CoreRubyStreamLife.hh"
#include "debug/CoreStreamAlloc.hh"
#include "debug/RubyStream.hh"
#include "debug/StreamAlias.hh"
#include "debug/StreamEngineBase.hh"
#include "debug/StreamThrottle.hh"

#define SE_WARN(format, args...)                                               \
  warn("[SE%d]: " format, this->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...)                                            \
  SE_DPRINTF_(StreamEngineBase, format, ##args)

#define DEBUG_TYPE StreamEngineBase
#include "stream_log.hh"

namespace gem5 {

namespace {
static std::string DEBUG_STREAM_NAME =
    "(particlefilter.c::415(.omp_outlined..2) 444 bb45 bb91::tmp96(load))";

bool isDebugStream(Stream *S) {
  return S->getStreamName() == DEBUG_STREAM_NAME;
}

} // namespace

std::vector<StreamEngine *> StreamEngine::GlobalStreamEngines;

StreamEngine *
StreamEngine::getStreamEngineAtCPU(GemForgeCPUDelegator::CPUTypeE cpuType,
                                   int cpuId) {
  for (auto se : GlobalStreamEngines) {
    if (se->cpuDelegator->cpuType == cpuType &&
        se->cpuDelegator->cpuId() == cpuId) {
      return se;
    }
  }
  return nullptr;
}

StreamEngine::StreamEngine(const Params &params)
    : GemForgeAccelerator(params), streamPlacementManager(nullptr),
      myParams(&params), isOracle(false), writebackCacheLine(nullptr),
      throttler(new StreamThrottler(params.throttling, this)) {

  this->isOracle = params.streamEngineIsOracle;
  this->defaultRunAheadLength = params.defaultRunAheadLength;
  this->currentTotalRunAheadLength = 0;
  this->totalRunAheadLength = params.totalRunAheadLength;
  this->totalRunAheadBytes = params.totalRunAheadBytes;
  this->enableLSQ = params.streamEngineEnableLSQ;
  this->enableCoalesce = params.streamEngineEnableCoalesce;
  this->enableMerge = params.streamEngineEnableMerge;
  this->enableStreamPlacement = params.streamEngineEnablePlacement;
  this->enableStreamPlacementOracle = params.streamEngineEnablePlacementOracle;
  this->enableStreamPlacementBus = params.streamEngineEnablePlacementBus;
  this->noBypassingStore = params.streamEngineNoBypassingStore;
  this->continuousStore = params.streamEngineContinuousStore;
  this->enablePlacementPeriodReset = params.streamEnginePeriodReset;
  this->placementLat = params.streamEnginePlacementLat;
  this->placement = params.streamEnginePlacement;
  this->enableStreamFloat = params.streamEngineEnableFloat;
  this->enableStreamFloatIndirect = params.streamEngineEnableFloatIndirect;
  this->enableStreamFloatPseudo = params.streamEngineEnableFloatPseudo;
  this->enableStreamFloatCancel = params.streamEngineEnableFloatCancel;

  auto streamFloatPolicy = std::make_unique<StreamFloatPolicy>(
      this, this->enableStreamFloat, params.enableFloatMem,
      params.enableFloatHistory, params.streamEngineFloatPolicy,
      params.floatLevelPolicy);
  this->floatController = std::make_unique<StreamFloatController>(
      this, std::move(streamFloatPolicy));

  this->ndcController = std::make_unique<StreamNDCController>(this);
  this->computeEngine = std::make_unique<StreamComputeEngine>(this, &params);
  this->regionController = std::make_unique<StreamRegionController>(this);
  this->rangeSyncController = std::make_unique<StreamRangeSyncController>(this);

  this->dataTrafficAccFix =
      std::make_unique<StreamDataTrafficAccumulator>(this, false /* floated */
      );
  this->dataTrafficAccFloat =
      std::make_unique<StreamDataTrafficAccumulator>(this, true /* floated */
      );

  this->initializeFIFO(this->totalRunAheadLength);
}

StreamEngine::~StreamEngine() {
  if (this->streamPlacementManager != nullptr) {
    delete this->streamPlacementManager;
    this->streamPlacementManager = nullptr;
  }

  // Clear all the allocated streams.
  for (auto &streamIdStreamPair : this->streamMap) {
    delete streamIdStreamPair.second;
    streamIdStreamPair.second = nullptr;
  }
  this->streamMap.clear();
  delete[] this->writebackCacheLine;
  this->writebackCacheLine = nullptr;

  for (auto iter = GlobalStreamEngines.begin();
       iter != GlobalStreamEngines.end(); ++iter) {
    if (*iter == this) {
      GlobalStreamEngines.erase(iter);
      break;
    }
  }
}

void StreamEngine::handshake(GemForgeCPUDelegator *_cpuDelegator,
                             GemForgeAcceleratorManager *_manager) {
  GemForgeAccelerator::handshake(_cpuDelegator, _manager);

  GlobalStreamEngines.push_back(this);

  LLVMTraceCPU *_cpu = nullptr;
  if (auto llvmTraceCPUDelegator =
          dynamic_cast<LLVMTraceCPUDelegator *>(_cpuDelegator)) {
    _cpu = llvmTraceCPUDelegator->cpu;
  }
  // assert(_cpu != nullptr && "Only work for LLVMTraceCPU so far.");
  this->cpu = _cpu;

  this->writebackCacheLine = new uint8_t[cpuDelegator->cacheLineSize()];
  if (this->enableStreamPlacement) {
    this->streamPlacementManager = new StreamPlacementManager(this);
  }

  // Set up the translation buffer.
  this->translationBuffer = std::make_unique<StreamTranslationBuffer<void *>>(
      cpuDelegator->getDataTLB(),
      [this](PacketPtr pkt, ThreadContext *tc, void *) -> void {
        this->cpuDelegator->sendRequest(pkt);
      },
      false /* AccessLastLevelTLBOnly */, true /* MustDoneInOrder */);

  // Set the name of DataTrafficAcc.
  this->dataTrafficAccFix->setName(this->manager->name() + ".se.dataAccFix");
  this->dataTrafficAccFloat->setName(this->manager->name() +
                                     ".se.dataAccFloat");
}

void StreamEngine::takeOverBy(GemForgeCPUDelegator *newCpuDelegator,
                              GemForgeAcceleratorManager *newManager) {
  GemForgeAccelerator::takeOverBy(newCpuDelegator, newManager);
  this->regionController->takeOverBy(newCpuDelegator);
}

void StreamEngine::regStats() {
  GemForgeAccelerator::regStats();
  assert(this->manager && "No handshake.");

#define scalar(stat, describe)                                                 \
  this->stat.name(this->manager->name() + (".se." #stat))                      \
      .desc(describe)                                                          \
      .prereq(this->stat)

  scalar(numConfigured, "Number of streams configured.");
  scalar(numStepped, "Number of streams stepped.");
  scalar(numUnstepped, "Number of streams unstepped.");
  scalar(numElementsAllocated, "Number of stream elements allocated.");
  scalar(numElementsUsed, "Number of stream elements used.");
  scalar(numCommittedStreamUser, "Number of committed StreamUser.");
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
  scalar(numFloated, "Number of floated streams.");
  scalar(numLLCSentSlice, "Number of LLC sent slices.");
  scalar(numLLCMigrated, "Number of LLC stream migration.");
  scalar(numMLCResponse, "Number of MLCStreamEngine response.");

  scalar(numScheduledComputation, "Number of scheduled computation in CoreSE.");
  scalar(numCompletedComputation, "Number of completed computation in CoreSE.");
  scalar(numCompletedComputeMicroOps,
         "Number of completed microops in CoreSE.");

#define complete_micro_op(Addr, Compute)                                       \
  scalar(numCompleted##Addr##Compute##MicroOps,                                \
         "Number of completed " #Addr " " #Compute " microops in CoreSE")

  complete_micro_op(Affine, LoadCompute);
  complete_micro_op(Affine, StoreCompute);
  complete_micro_op(Affine, AtomicCompute);
  complete_micro_op(Affine, Reduce);
  complete_micro_op(Affine, Update);
  complete_micro_op(Indirect, LoadCompute);
  complete_micro_op(Indirect, StoreCompute);
  complete_micro_op(Indirect, AtomicCompute);
  complete_micro_op(Indirect, Reduce);
  complete_micro_op(Indirect, Update);
  complete_micro_op(PointerChase, LoadCompute);
  complete_micro_op(PointerChase, StoreCompute);
  complete_micro_op(PointerChase, AtomicCompute);
  complete_micro_op(PointerChase, Reduce);
  complete_micro_op(PointerChase, Update);
  complete_micro_op(MultiAffine, LoadCompute);
  complete_micro_op(MultiAffine, StoreCompute);
  complete_micro_op(MultiAffine, AtomicCompute);
  complete_micro_op(MultiAffine, Reduce);
  complete_micro_op(MultiAffine, Update);
#undef complete_micro_op

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
  this->numInflyStreamRequestDist.init(0, 16, 1)
      .name(this->manager->name() + ".stream.numInflyStreamDist")
      .desc("Distribution of infly stream requests.")
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

  this->dataTrafficAccFix->regStats();
  this->dataTrafficAccFloat->regStats();
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
    if (configuredStreams * 3 > this->totalRunAheadLength) {
      panic("Too many streams configuredStreams for %s %d, FIFOSize %d.\n",
            infoRelativePath.c_str(), configuredStreams,
            this->totalRunAheadLength);
    }
  }

  // if (this->numFreeFIFOEntries < configuredStreams) {
  //   // Not enough free entries for each stream.
  //   SE_DPRINTF("[CanNotDispatch] NoFreeFIFO %s.\n", streamRegion.region());
  //   return false;
  // }

  // // Check that allocSize < maxSize.
  // if (this->enableCoalesce) {
  //   for (const auto &streamId : streamRegion.coalesced_stream_ids()) {
  //     auto iter = this->streamMap.find(streamId);
  //     if (iter != this->streamMap.end()) {
  //       // Check if we have quota for this stream.
  //       auto S = iter->second;
  //       if (S->getAllocSize() == S->maxSize) {
  //         // No more quota.
  //         SE_DPRINTF("[CanNotDispatch] AllocSize = MaxSize %d %s.\n",
  //                    S->getAllocSize(), streamRegion.region());
  //         return false;
  //       }
  //     }
  //   }
  // } else {
  //   for (const auto &streamInfo : streamRegion.streams()) {
  //     auto streamId = streamInfo.id();
  //     auto iter = this->streamMap.find(streamId);
  //     if (iter != this->streamMap.end()) {
  //       // Check if we have quota for this stream.
  //       auto S = iter->second;
  //       if (S->getAllocSize() == S->maxSize) {
  //         // No more quota.
  //         return false;
  //       }
  //     }
  //   }
  // }
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

  SE_DPRINTF("Dispatch StreamConfig for %s, %s.\n", streamRegion.region(),
             args.infoRelativePath);

  // Initialize all the streams if this is the first time we encounter the
  // loop.
  this->tryInitializeStreams(streamRegion);

  const auto &configStreams = this->getConfigStreamsInRegion(streamRegion);
  for (auto &S : configStreams) {
    S->statistic.numConfigured++;

    // Notify the stream.
    S->configure(args.seqNum, args.tc);
  }

  // Handle dynamic stream dependence.
  // This is split out from S->configure() to ensure all DynS are created
  // before we handle dynamic dependence.
  for (auto &S : configStreams) {
    auto &dynS = S->getLastDynStream();
    dynS.addBaseDynStreams();
    dynS.addOuterDepDynStreams(args.outerSE, args.outerSeqNum);
    if (S->stepRootStream == S) {
      dynS.addStepStreams();
    }
  }

  // Notify StreamRegionController.
  this->regionController->dispatchStreamConfig(args);
}

void StreamEngine::executeStreamConfig(const StreamConfigArgs &args) {

  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);

  SE_DPRINTF("Execute StreamConfig for %s.\n", streamRegion.region());

  const auto &configStreams = this->getConfigStreamsInRegion(streamRegion);

  /**
   * First notify the stream. This will set up the addr gen function.
   * This has to be done before trying to offload stream.
   */
  for (auto &S : configStreams) {
    const StreamConfigArgs::InputVec *inputVec = nullptr;
    if (args.inputMap) {
      inputVec = &(args.inputMap->at(S->staticId));
    }
    S->executeStreamConfig(args.seqNum, inputVec);
  }

  /**
   * Then we try to compute the reuse between streams.
   * This has to be done after initializing the addr gen function.
   */
  std::list<DynStream *> configDynStreams;
  for (auto &S : configStreams) {
    auto &dynS = S->getDynStream(args.seqNum);
    dynS.configureBaseDynStreamReuse();
    configDynStreams.push_back(&dynS);
  }

  /**
   * Notify StreamRegionController.
   * This must happen before FloatStreams, as it will set InnerTripCount.
   */
  this->regionController->executeStreamConfig(args);

  /**
   * Then we try to float streams.
   */
  this->floatController->floatStreams(args, streamRegion, configDynStreams);

  /**
   * Then we determine the step count. This must be after floatStreams as it
   * only applies to streams floated.
   */
  this->regionController->determineStepElemCount(args);

  /**
   * We also try to enable fine-grained near-data computing.
   */
  if (this->myParams->enableFineGrainedNearDataComputing) {
    this->ndcController->offloadStreams(args, streamRegion, configDynStreams);
  }

  /**
   * If not FloatAsNDC, add to issuing List.
   */
  for (auto dynS : configDynStreams) {
    if (!dynS->isFloatedAsNDC()) {
      this->addIssuingDynS(dynS);
    }
  }
}

void StreamEngine::commitStreamConfig(const StreamConfigArgs &args) {
  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);

  SE_DPRINTF("Commit StreamConfig for %s.\n", streamRegion.region());

  const auto &configStreams = this->getConfigStreamsInRegion(streamRegion);

  /**
   * First notify the stream. This will set up the addr gen function.
   * This has to be done before trying to offload stream.
   */
  for (auto &S : configStreams) {
    S->commitStreamConfig(args.seqNum);
  }

  /**
   * We can now commit offload decision.
   */
  this->floatController->commitFloatStreams(args, configStreams);

  /**
   * Inform the RegionController.
   */
  this->regionController->commitStreamConfig(args);

  /**
   * If FloatAsNDC, add to issuing List.
   */
  for (auto S : configStreams) {
    auto &dynS = S->getDynStream(args.seqNum);
    if (dynS.isFloatedAsNDC()) {
      this->addIssuingDynS(&dynS);
    }
  }
}

void StreamEngine::rewindStreamConfig(const StreamConfigArgs &args) {

  const auto &infoRelativePath = args.infoRelativePath;
  const auto &configSeqNum = args.seqNum;
  const auto &streamRegion = this->getStreamRegion(infoRelativePath);

  SE_DPRINTF("Rewind StreamConfig %s.\n", infoRelativePath);

  // Notify StreamRegionController.
  this->regionController->rewindStreamConfig(args);

  const auto &configStreams = this->getConfigStreamsInRegion(streamRegion);

  // First we need to rewind any floated streams.
  this->floatController->rewindFloatStreams(args, configStreams);

  for (auto &S : configStreams) {
    // Remove from the IssueList.
    auto &dynS = S->getDynStream(configSeqNum);
    this->removeIssuingDynS(&dynS);

    // First release all element.
    // This is to ensure that StepRooDynS is still here.
    while (dynS.hasUnsteppedElem()) {
      this->releaseElementUnstepped(dynS);
    }
  }

  for (auto &S : configStreams) {
    // Finally rewind the DynStream.
    S->rewindStreamConfig(configSeqNum);
  }

  this->numInflyStreamConfigurations--;
}

bool StreamEngine::canDispatchStreamStep(const StreamStepArgs &args) const {
  // We check two things:
  // 1. We have an unstepped element.
  auto stepStream = this->getStream(args.stepStreamId);
  for (auto S : this->getStepStreamList(stepStream)) {
    if (!S->hasUnsteppedElement(args.dynInstanceId)) {
      return false;
    }
  }
  return true;
}

void StreamEngine::dispatchStreamStep(const StreamStepArgs &args) {
  /**
   * For all the streams get stepped, increase the stepped pointer.
   */
  assert(this->canDispatchStreamStep(args) &&
         "canDispatchStreamStep assertion failed.");
  this->numStepped++;
  this->numSteppedSinceLastCheck++;

  auto stepStream = this->getStream(args.stepStreamId);

  for (auto S : this->getStepStreamList(stepStream)) {
    assert(S->isConfigured() && "Stream should be configured to be stepped.");
    DynStream *dynS = nullptr;
    if (args.dynInstanceId == DynStreamId::InvalidInstanceId) {
      dynS = &(S->getFirstAliveDynStream());
    } else {
      dynS = &(S->getDynStreamByInstance(args.dynInstanceId));
    }
    dynS->stepElement(false /* isEnd */);
  }
  if (isDebugStream(stepStream)) {
  }
}

bool StreamEngine::canCommitStreamStep(const StreamStepArgs &args) {
  auto stepStream = this->getStream(args.stepStreamId);

  const auto &stepStreams = this->getStepStreamList(stepStream);

  for (auto S : stepStreams) {
    /**
     * Normally commit happens in-order, we know it's the FirstDynStream.
     * Except for NestStream with EliminatedLoop, which may be independently
     * stepped by each dynamic stream.
     */
    DynStream *dynS = &S->getFirstDynStream();
    if (args.dynInstanceId != DynStreamId::InvalidInstanceId) {
      dynS = &S->getDynStreamByInstance(args.dynInstanceId);
    }
    if (!dynS->configExecuted) {
      DYN_S_DPRINTF(dynS->dynStreamId,
                    "[CanNotCommitStep] Config Not Executed.\n");
      return false;
    }
    auto stepElem = dynS->tail->next;
    if (dynS->hasTotalTripCount()) {
      if (stepElem->isLastElement()) {
        S_ELEMENT_PANIC(stepElem, "StreamStep for LastElement.");
      }
    }
    /**
     * For floating streams enabled StoreFunc, we have to check for StreamAck.
     * However, if we have Range-Sync enabled, we should commit it directly.
     */
    if (S->getEnabledStoreFunc()) {
      if (stepElem->isElemFloatedToCache() && !dynS->shouldCoreSEIssue() &&
          !dynS->shouldRangeSync()) {
        if (!dynS->cacheAckedElements.count(stepElem->FIFOIdx.entryIdx)) {
          S_ELEMENT_DPRINTF(stepElem, "[CanNotCommitStep] No Ack.\n");
          return false;
        }
      }
      if (dynS->isFloatedAsNDC()) {
        if (!dynS->cacheAckedElements.count(stepElem->FIFOIdx.entryIdx)) {
          S_ELEMENT_DPRINTF(stepElem, "[CanNotCommitStep] No Ack from NDC.\n");
          return false;
        }
      }
    }
    /**
     * For LoopElimInCoreStoreCmpS, we have to wait until the elem is issued
     * so that the write is sent to cache. If not issued but ValueReady, we add
     * to issuing list.
     */
    if (dynS->isLoopElimInCoreStoreCmpS()) {
      if (!stepElem->isReqIssued()) {
        auto cmpValReady = stepElem->isComputeValueReady();
        S_ELEMENT_DPRINTF(stepElem,
                          "[CanNotCommitStep] ReqNotIssue for "
                          "LoopElimInCoreStoreCmpS CmpValReady %d.\n",
                          cmpValReady);
        if (cmpValReady) {
          this->addIssuingDynS(dynS);
        }
        return false;
      }
    }
    if (dynS->isLoopElimInCoreUpdateS()) {
      auto cmpValReady = stepElem->isComputeValueReady();
      if (!cmpValReady) {
        S_ELEMENT_DPRINTF(
            stepElem,
            "[CanNotCommitStep] LoopElimInCoreUpdateS CmpValReady %d.\n",
            cmpValReady);
        if (!stepElem->scheduledComputation &&
            stepElem->checkValueBaseElemsValueReady()) {
          this->addIssuingDynS(dynS);
        }
        return false;
      }
    }
    if (!dynS->areNextBackDepElementsReady(stepElem)) {
      S_ELEMENT_DPRINTF(stepElem,
                        "[CanNotCommitStep] BackDepElement Unready.\n");
      return false;
    }
    if (S->isReduction() || S->isPointerChaseIndVar()) {
      auto stepNextElement = stepElem->next;
      if (!stepNextElement) {
        /**
         * There is one exception -- we are the FirstFloatElem, where we only
         * check that we are value ready.
         * NOTE: Here we just use FirstFloatElemIdx, not
         * AdjustedFirstFloatElemIdx.
         */
        if (stepElem->FIFOIdx.entryIdx == dynS->getFirstFloatElemIdx()) {
          if (!stepElem->isValueReady) {
            S_ELEMENT_DPRINTF(stepElem, "[CanNotCommitStep] Reduce/PtrChaseIV "
                                        "FirstFloatElem Not ValueReady.\n");
            return false;
          }
        } else {
          S_ELEMENT_DPRINTF(
              stepElem,
              "[CanNotCommitStep] No Reduction/PtrChaseIV NextElement.\n");
          return false;
        }
      } else {
        if (stepNextElement->isElemFloatedToCache()) {
        } else {
          // If not offloaded, The next steped element should be ValueReady.
          if (!stepNextElement->isValueReady) {
            S_ELEMENT_DPRINTF(stepElem,
                              "[CanNotCommitStep] Reduction/PtrChaseIV "
                              "NextElement %llu not ValueReady.\n",
                              stepNextElement->FIFOIdx.entryIdx);
            return false;
          }
        }
      }
    }
    if (S->isLoadStream() && stepElem->shouldIssue() && !S->hasCoreUser() &&
        !S->backDepStreams.empty()) {
      /**
       * S is a issuing load stream without no core user, but has backDepS.
       * We have to make sure the element is value ready so
       * that the backDepS is correctly performed.
       */
      for (const auto &backDepEdge : dynS->backDepEdges) {
        auto backDepS = this->getStream(backDepEdge.depStaticId);
        const auto &backDepDynS = backDepS->getDynStream(dynS->configSeqNum);
        auto backDepElem =
            backDepDynS.getElemByIdx(stepElem->FIFOIdx.entryIdx + 1);
        if (!backDepElem) {
          S_ELEMENT_DPRINTF(
              stepElem, "[CanNotCommitStep] No BackDepElem %s Elem %ld.\n",
              backDepDynS.dynStreamId, stepElem->FIFOIdx.entryIdx + 1);
          return false;
        }
        if (!backDepElem->isValueReady) {
          S_ELEMENT_DPRINTF(stepElem,
                            "[CanNotCommitStep] Unready BackDepElem %s.\n",
                            backDepElem->FIFOIdx);
          return false;
        }
      }
    }
    /**
     * Since we coalesce for continuous DirectMemStream, we delay releasing
     * stepped element here if the next element is not addr ready. This is
     * to ensure that it is correctly coalesced.
     * This is disabled if the stream delays issue until reaches the FIFO head.
     * Or stream is not coalesced.
     */
    if (S->isDirectMemStream() && !S->isDelayIssueUntilFIFOHead() &&
        dynS->shouldCoreSEIssue() && !dynS->isFloatedAsNDC() &&
        this->shouldCoalesceContinuousDirectMemStreamElement(stepElem)) {
      auto stepNextElem = stepElem->next;
      if (!stepNextElem) {
        S_ELEMENT_DPRINTF(
            stepElem, "[CanNotCommitStep] No NextElem CoreIssue DirectMemS.\n");
        return false;
      }
      if (!stepNextElem->isAddrReady() && stepNextElem->shouldIssue()) {
        S_ELEMENT_DPRINTF(stepElem, "[CanNotCommitStep] NextElem not AddrReady "
                                    "CoreIssue DirectMemS.\n");
        return false;
      }
    }
  }

  // We have one more condition for range-based check.
  if (auto noRangeDynS = this->rangeSyncController->getNoRangeDynS()) {
    SE_DPRINTF("[CanNotCommitStep] No Range for %s%llu.\n",
               noRangeDynS->dynStreamId,
               this->rangeSyncController->getCheckElemIdx(noRangeDynS));
    return false;
  }
  return true;
}

void StreamEngine::commitStreamStep(const StreamStepArgs &args) {
  auto stepStream = this->getStream(args.stepStreamId);

  const auto &stepStreams = this->getStepStreamList(stepStream);

  /**
   * Notify the StreamDataTrafficAccumulator to estimate for data traffic.
   * Only do this if we have IdeaCache in GemForgeCPUDelegator.
   */
  if (this->cpuDelegator->ideaCache) {
    this->dataTrafficAccFix->commit(stepStreams);
    this->dataTrafficAccFloat->commit(stepStreams);
  }

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
    DynStream *dynS = nullptr;
    if (args.dynInstanceId == DynStreamId::InvalidInstanceId) {
      dynS = &(S->getFirstDynStream());
    } else {
      dynS = &(S->getDynStreamByInstance(args.dynInstanceId));
    }
    this->releaseElementStepped(dynS, false /* isEnd */, true /* doThrottle */);
  }

  // ! Do not allocate here.
  // ! StreamRegionController::allocateElements() will handle it.

  if (isDebugStream(stepStream)) {
  }
}

void StreamEngine::rewindStreamStep(const StreamStepArgs &args) {
  this->numUnstepped++;
  auto stepStream = this->getStream(args.stepStreamId);
  for (auto S : this->getStepStreamList(stepStream)) {
    assert(S->isConfigured() && "Stream should be configured to be stepped.");
    DynStream *dynS = nullptr;
    if (args.dynInstanceId == DynStreamId::InvalidInstanceId) {
      dynS = &(S->getFirstAliveDynStream());
    } else {
      dynS = &(S->getDynStreamByInstance(args.dynInstanceId));
    }
    dynS->unstepElement();
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
    if (!S->isConfigured()) {
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
    if (element->stream->getStreamType() != ::LLVM::TDG::StreamInfo_Type_LD) {
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

int StreamEngine::createStreamUserLSQCallbacks(
    const StreamUserArgs &args, GemForgeLSQCallbackList &callbacks) {
  auto seqNum = args.seqNum;
  auto &elementSet = this->userElementMap.at(seqNum);
  auto numCallbacks = 0;
  for (auto &element : elementSet) {
    if (element == nullptr) {
      continue;
    }
    auto S = element->stream;
    bool pushToLQ = false;
    bool pushToSQ = false;
    if (S->isLoadStream()) {
      if (element->firstUserSeqNum == seqNum && !args.isStore) {
        // Insert into the load queue if this is the first user.
        pushToLQ = true;
      }
      if (S->isUpdateStream() && !element->isElemFloatedToCache()) {
        // Insert into the store queue if this is the first StreamStore.
        if (element->firstStoreSeqNum == seqNum) {
          pushToSQ = true;
        }
      }
    } else if (S->isAtomicComputeStream()) {
      if (!element->isElemFloatedToCache() && !element->isElemFloatedAsNDC()) {
        // We skip LSQ for Offloaded AtomicComputeStream.
        if (element->firstUserSeqNum == seqNum) {
          pushToLQ = true;
        }
      }
    } else if (S->isStoreComputeStream()) {
      if (!element->isElemFloatedToCache() && !element->isElemFloatedAsNDC()) {
        if (element->firstUserSeqNum == seqNum) {
          if (!this->enableLSQ) {
            S_ELEMENT_PANIC(element,
                            "StoreStream executed at core requires LSQ.");
          }
          pushToSQ = true;
        }
      }
    }
    if (this->enableLSQ) {
      if (pushToLQ) {
        assert(numCallbacks < callbacks.size() && "LQCallback overflows.");
        callbacks.at(numCallbacks) = std::make_unique<StreamLQCallback>(
            element, seqNum, args.pc, args.usedStreamIds);
        numCallbacks++;
      }
      if (pushToSQ) {
        assert(numCallbacks < callbacks.size() && "SQCallback overflows.");
        callbacks.at(numCallbacks) = std::make_unique<StreamSQCallback>(
            element, seqNum, args.pc, args.usedStreamIds);
        numCallbacks++;
      }
    }
  }
  return numCallbacks;
}

bool StreamEngine::hasUnsteppedElement(const StreamUserArgs &args) {
  for (const auto &streamId : args.usedStreamIds) {
    auto S = this->getStream(streamId);
    if (!S->hasUnsteppedElement(DynStreamId::InvalidInstanceId)) {
      return false;
    }
  }
  return true;
}

bool StreamEngine::hasIllegalUsedLastElement(const StreamUserArgs &args) {
  for (const auto &streamId : args.usedStreamIds) {
    auto S = this->getStream(streamId);
    if (!S->isConfigured()) {
      continue;
    }
    if (S->isInnerFinalValueUsedByCore()) {
      // The only exception is for ReductionStream, whose LastElement is used to
      // convey back the final value. It should had CoreNeedFinalValue set.
      continue;
    }
    auto &dynS = S->getFirstAliveDynStream();
    if (!dynS.configExecuted) {
      continue;
    }
    auto element = dynS.getFirstUnsteppedElem();
    assert(element && "Has no unstepped element.");
    if (element->isLastElement()) {
      S_ELEMENT_DPRINTF(element, "Used LastElement total %d next %s.\n",
                        dynS.getTotalTripCount(), dynS.FIFOIdx);
      return true;
    }
  }
  return false;
}

bool StreamEngine::canDispatchStreamUser(const StreamUserArgs &args) {
  if (!this->hasUnsteppedElement(args)) {
    return false;
  }
  for (const auto &streamId : args.usedStreamIds) {
    auto S = this->getStream(streamId);
    auto dynS = S->tryGetFirstAliveDynStream();
    if (!dynS) {
      /**
       * It's possible that due to NestStream, the stream has not been
       * configured.
       */
      return false;
    }
    /**
     * Additional condition for StoreStream with enabled StoreFunc, we
     * wait for config to be executed to avoid creating SQCallback for
     * floating store streams.
     */
    if (!dynS->configExecuted) {
      return false;
    }
    /**
     * For LoopEliminatedStream, we have to wait until:
     * 1. The second last element if we are using SecondFinalValue.
     * 2. The last element otherwise.
     *
     * In PartialElim:
     * for i = 0 : N
     *   for j = 0 : M
     *     s += x;
     *   s_load(s);
     *
     * After eliminate the InnerLoop, we have to make sure s_load()
     * uses the correct element (multiple of M). We leverage the fact
     * that there will only be one FinalUser, and block dispatch if
     * the Element already has a User.
     */
    if (S->isLoopEliminated()) {
      // We already checked that we have UnsteppedElement.
      auto elem = dynS->getFirstUnsteppedElem();
      if (S->isInnerSecondFinalValueUsedByCore()) {
        if (!elem->isInnerSecondLastElem()) {
          S_ELEMENT_DPRINTF(elem,
                            "Is Not InnerSecondLastElem. InnerTripCount %lu.\n",
                            dynS->getInnerTripCount());
          return false;
        }
        if (elem->isFirstUserDispatched()) {
          S_ELEMENT_DPRINTF(elem, "InnerSecondLastElem already has User.\n");
          return false;
        }
      } else {
        if (!S->isInnerFinalValueUsedByCore()) {
          DYN_S_PANIC(dynS->dynStreamId,
                      "LoopEliminated Stream with User should have FinalValue "
                      "or SecondFinalValue needed.");
        }
        if (!elem->isInnerLastElem()) {
          S_ELEMENT_DPRINTF(elem, "Is Not InnerLastElem. InnerTripCount %lu.\n",
                            dynS->getInnerTripCount());
          return false;
        }
        if (elem->isFirstUserDispatched()) {
          S_ELEMENT_DPRINTF(elem, "InnerLastElem already has User.\n");
          return false;
        }
      }
    }
  }
  return true;
}

void StreamEngine::dispatchStreamUser(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  SE_DPRINTF("Dispatch StreamUser sn:%llu.\n", seqNum);
  assert(this->userElementMap.count(seqNum) == 0);
  assert(this->hasUnsteppedElement(args) && "Don't have used elements.\n");

  auto &elementSet =
      this->userElementMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(seqNum),
                   std::forward_as_tuple())
          .first->second;

  for (const auto &streamId : args.usedStreamIds) {
    auto S = this->getStream(streamId);
    if (!S->hasCoreUser()) {
      // There are exceptions for this sanity check.
      // 1. Some streams may use the LastValue (e.g. ReductionStream) or
      // SecondLastValue.
      // 2. StoreComputeStream/UpdateStream use StreamStore inst to get the
      // address and value from the SE so the core can finally write back.
      // 3. AtomicComputeStream always have a StreamAtomic inst as a place
      // holder.
      if (!S->isInnerFinalValueUsedByCore() &&
          !S->isInnerSecondFinalValueUsedByCore() &&
          !S->isStoreComputeStream() && !S->isAtomicComputeStream() &&
          !S->isUpdateStream()) {
        S_PANIC(S, "Try to use a stream with no core user.");
      }
    }

    /**
     * It is possible that the stream is unconfigured (out-loop use).
     * In such case we assume it's ready and use a nullptr as a special
     * element
     */
    if (!S->isConfigured()) {
      elementSet.insert(nullptr);
    } else {

      assert(!S->isMerged() &&
             "Merged stream should never be used by the core.");

      auto elem = S->getFirstUnsteppedElement();
      // Mark the first user sequence number.
      if (!elem->isFirstUserDispatched()) {
        elem->firstUserSeqNum = seqNum;
        // Remember the first core user pc.
        S->setFirstCoreUserPC(args.pc);
        if (S->trackedByPEB() && elem->isReqIssued()) {
          // The element should already be in peb, remove it.
          this->peb.removeElement(elem);
        }
      }
      if (!elem->isFirstStoreDispatched() && args.isStore) {
        // Remember the first StreamStore.
        elem->firstStoreSeqNum = seqNum;
      }
      S_ELEMENT_DPRINTF(elem, "Dispatch StreamUser %llu.\n", seqNum);
      elementSet.insert(elem);
      // Construct the elementUserMap.
      this->elementUserMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(elem),
                   std::forward_as_tuple())
          .first->second.insert(seqNum);
    }
  }
}

bool StreamEngine::areUsedStreamsReady(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  assert(this->userElementMap.count(seqNum) != 0);

  bool ready = true;
  bool allStreamLoopEliminated = true;
  bool allDynStreamConfigCommitted = true;
  for (auto &elem : this->userElementMap.at(seqNum)) {
    if (elem == nullptr) {
      /**
       * Sometimes there is use after stream end,
       * in such case we assume the element is copied to register and
       * is ready.
       */
      continue;
    }
    auto S = elem->stream;
    if (!S->isLoopEliminated()) {
      allStreamLoopEliminated = false;
    } else {
      if (S->isNestStream()) {
        const auto &innerStaticRegion =
            this->regionController->getStaticRegion(S);
        const auto &outerStaticRegion = this->regionController->getStaticRegion(
            innerStaticRegion.nestConfig.outerRegion);
        if (!outerStaticRegion.allStreamsLoopEliminated) {
          allStreamLoopEliminated = false;
        }
      }
    }
    if (!elem->dynS->configCommitted) {
      allDynStreamConfigCommitted = false;
    }
    // Floating Store/AtomicComputeStream will only check for Ack when stepping.
    // This also true for floating UpdateStream's SQCallback.
    if (elem->isElemFloatedToCache() || elem->isElemFloatedAsNDC()) {
      if (S->isStoreComputeStream()) {
        continue;
      }
      if (S->isUpdateStream() && args.isStore) {
        continue;
      }
      if (S->isAtomicComputeStream() && !S->hasCoreUser()) {
        continue;
      }
    }
    if (S->isUpdateStream() && args.isStore) {
      /**
       * Special case for UpdateStream's SQCallback:
       * We check the UpdateValue, not the normal value.
       */
      if (!(elem->isAddrReady() && elem->checkUpdateValueReady())) {
        S_ELEMENT_DPRINTF(elem, "NotReady: AddrReady %d UpdateValueReady %d.\n",
                          elem->isAddrReady(), elem->isUpdateValueReady());
        ready = false;
      }
    } else if (S->isLoadComputeStream() && !elem->isElemFloatedToCache()) {
      /**
       * Special case for not floated LoadComputeStream, where we should check
       * for LoadComputeValue.
       */
      if (!(elem->isAddrReady() &&
            elem->checkLoadComputeValueReady(true /* CheckedByCore */))) {
        S_ELEMENT_DPRINTF(elem,
                          "NotReady: AddrReady %d LoadComputeValueReady %d.\n",
                          elem->isAddrReady(), elem->isUpdateValueReady());
        ready = false;
      }
    } else {
      if (!(elem->isAddrReady() &&
            elem->checkValueReady(true /* CheckedByCore */))) {
        S_ELEMENT_DPRINTF(elem, "NotReady: AddrReady %d ValueReady %d.\n",
                          elem->isAddrReady(), elem->isValueReady);
        ready = false;
      }
    }
  }

  if (this->myParams->yieldCoreWhenBlocked && !ready &&
      allStreamLoopEliminated && allDynStreamConfigCommitted) {
    SE_DPRINTF("StreamNotReady: Yield.\n");
    this->yieldCPU();
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
    [[maybe_unused]] auto inserted =
        streamToElementMap
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
    auto elem = streamToElementMap.at(S);
    auto size = S->getCoreElementSize();
    args.values->emplace_back();
    args.valueSizes->emplace_back(size);
    /**
     * Make sure we zero out the data.
     */
    args.values->back().fill(0);
    if (elem->isElemFloatedToCache() || elem->isElemFloatedAsNDC()) {
      /**
       * There are certain cases we are not really return the value.
       * 1. StreamStore does not really return any value.
       * 2. Floating AtomicComputeStream.
       */
      if (S->isStoreStream()) {
        continue;
      }
      if (S->isAtomicComputeStream() && !S->hasCoreUser()) {
        continue;
      }
    }
    /**
     * Read in the value.
     * If this is a unfloated LoadComputeStream, we should read the
     * LoadComputeValue.
     */
    if (S->isLoadComputeStream() && !elem->isElemFloatedToCache()) {
      elem->getLoadComputeValue(args.values->back().data(),
                                StreamUserArgs::MaxElementSize);
    } else {
      elem->getValueByStreamId(streamId, args.values->back().data(),
                               StreamUserArgs::MaxElementSize);
    }
    S_ELEMENT_DPRINTF(
        elem, "Execute StreamUser %lu, Value %s.\n", seqNum,
        GemForgeUtils::dataToString(args.values->back().data(), size));
  }
}

void StreamEngine::commitStreamUser(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  SE_DPRINTF("Commit StreamUser %lu.\n", seqNum);
  assert(this->userElementMap.count(seqNum) && "UserElementMap not correct.");
  // Remove the entry from the elementUserMap.
  for (auto elem : this->userElementMap.at(seqNum)) {
    /**
     * As a hack, we use nullptr to represent an out-of-loop use.
     * TODO: Fix this.
     */
    if (!elem) {
      continue;
    }
    S_ELEMENT_DPRINTF(elem, "Commit StreamUser %lu.\n", seqNum);

    auto S = elem->getStream();
    bool isActuallyUsed = true;
    if (elem->isElemFloatedToCache() || elem->isElemFloatedAsNDC()) {
      if (S->isStoreComputeStream()) {
        isActuallyUsed = false;
      }
      if ((S->isUpdateStream() || S->isAtomicComputeStream()) &&
          !S->hasCoreUser()) {
        isActuallyUsed = false;
      }
    }
    if (S->isUpdateStream() && args.isStore) {
      isActuallyUsed = false;
      if (!elem->isElemFloatedToCache()) {
        if (!elem->isUpdateValueReady()) {
          S_ELEMENT_PANIC(
              elem,
              "Commit StoreUser for UpdateStream, but UpdateValue not ready.");
        }
      }
    }
    if (!elem->isValueReady) {
      // The only exception is the Store/AtomicComputeStream is floated,
      // as well as the StreamStore to UpdateStream.
      if (isActuallyUsed) {
        S_ELEMENT_PANIC(elem, "Commit user, but value not ready.");
      }
    }

    /**
     * Sanity check that no faulted block is used.
     */
    if (isActuallyUsed) {
      for (auto streamId : args.usedStreamIds) {
        // Check if this streamId corresponding to S.
        if (this->getStream(streamId) != S) {
          continue;
        }
        auto vaddr = elem->addr;
        int32_t size = elem->size;
        // Handle offset for coalesced stream.
        int32_t offset;
        S->getCoalescedOffsetAndMemSize(streamId, offset, size);
        vaddr += offset;
        if (elem->isValueFaulted(vaddr, size)) {
          S_ELEMENT_PANIC(elem, "Commit user of faulted value.");
        }
      }
    }

    /**
     * Sanity check that the FinalValueUser really used the correct NestStream
     * instance. The used InnerS should be configured by the current header
     * element of the OuterS.
     */
    if (S->isInnerFinalValueUsedByCore() && elem->isInnerLastElem() &&
        S->isLoopEliminated() && S->isNestStream()) {
      const auto &innerStaticRegion =
          this->regionController->getStaticRegion(S);
      const auto &outerStaticRegion = this->regionController->getStaticRegion(
          innerStaticRegion.nestConfig.outerRegion);
      assert(!outerStaticRegion.dynRegions.empty());
      const auto &outerDynRegion = outerStaticRegion.dynRegions.front();
      auto innerConfigSeqNum = elem->dynS->configSeqNum;
      auto outerS = outerStaticRegion.streams.front();
      const auto &outerDynS = outerS->getDynStream(outerDynRegion.seqNum);
      auto outerFirstElem = outerDynS.getFirstElem();
      __attribute__((unused)) bool foundNestDynRegion = false;
      for (const auto &nestConfig : outerDynRegion.nestConfigs) {
        if (nestConfig.staticRegion != &innerStaticRegion) {
          // This is not our NestConfig.
          continue;
        }
        for (const auto &nestDynRegion : nestConfig.nestDynRegions) {
          if (nestDynRegion.configSeqNum == innerConfigSeqNum) {
            // We found it.
            foundNestDynRegion = true;
            if (nestDynRegion.outerElemIdx !=
                outerFirstElem->FIFOIdx.entryIdx) {
              S_ELEMENT_PANIC(elem,
                              "Mismatch between ElimNest InnerS and OuterS. "
                              "OuterHeadElem %s OuterConfigElemIdx %lu.\n",
                              outerFirstElem->FIFOIdx,
                              nestDynRegion.outerElemIdx);
            }
          }
        }
      }
      assert(foundNestDynRegion);
    }

    auto &userSet = this->elementUserMap.at(elem);
    panic_if(!userSet.erase(seqNum), "Not found in userSet.");
  }
  // Remove the entry in the userElementMap.
  this->userElementMap.erase(seqNum);
  this->numCommittedStreamUser++;
}

void StreamEngine::rewindStreamUser(const StreamUserArgs &args) {
  auto seqNum = args.seqNum;
  for (auto elem : this->userElementMap.at(seqNum)) {
    // The element should be in unstepped state.
    assert(!elem->isStepped && "Rewind user of stepped element.");
    S_ELEMENT_DPRINTF(elem, "Rewind StreamUser %lu.\n", seqNum);
    if (elem->firstUserSeqNum == seqNum) {
      // I am the first user.
      elem->firstUserSeqNum = LLVMDynamicInst::INVALID_SEQ_NUM;
      // Check if the element should go back to PEB.
      if (elem->stream->trackedByPEB() && elem->isReqIssued()) {
        this->peb.addElement(elem);
      }
    }
    if (elem->firstStoreSeqNum == seqNum) {
      // I am the first store.
      elem->firstStoreSeqNum = LLVMDynamicInst::INVALID_SEQ_NUM;
    }
    // Remove the entry from the elementUserMap.
    auto &userSet = this->elementUserMap.at(elem);
    panic_if(!userSet.erase(seqNum), "Not found in userSet.");
  }
  // Remove the entry in the userElementMap.
  this->userElementMap.erase(seqNum);
}

bool StreamEngine::canDispatchStreamEnd(const StreamEndArgs &args) {
  return this->regionController->canDispatchStreamEnd(args);
}

void StreamEngine::dispatchStreamEnd(const StreamEndArgs &args) {
  this->regionController->dispatchStreamEnd(args);
}

bool StreamEngine::canExecuteStreamEnd(const StreamEndArgs &args) {
  return this->regionController->canExecuteStreamEnd(args);
}

void StreamEngine::rewindStreamEnd(const StreamEndArgs &args) {
  this->regionController->rewindStreamEnd(args);
}

bool StreamEngine::canCommitStreamEnd(const StreamEndArgs &args) {
  return this->regionController->canCommitStreamEnd(args);
}

void StreamEngine::commitStreamEnd(const StreamEndArgs &args) {
  this->regionController->commitStreamEnd(args);
}

bool StreamEngine::canStreamStoreDispatch(const StreamStoreInst *inst) const {
  /**
   * * The only requirement about the SQ is already handled in the CPU.
   */
  return true;
}

std::list<std::unique_ptr<GemForgeSQDeprecatedCallback>>
StreamEngine::createStreamStoreSQCallbacks(StreamStoreInst *inst) {
  std::list<std::unique_ptr<GemForgeSQDeprecatedCallback>> callbacks;
  if (!this->enableLSQ) {
    return callbacks;
  }
  // ! This is the old implementation of GemForgeSQDeprecatedCallbacks. Not used
  // any more.
  assert(false && "We are moving to a new implemenation of StreamStore.");
  // // So far we only support LSQ for LLVMTraceCPU.
  // assert(cpuDelegator->cpuType == GemForgeCPUDelegator::CPUTypeE::LLVM_TRACE
  // &&
  //        "LSQ only works for LLVMTraceCPU.");
  // // Find the element to be stored.
  // StreamElement *storeElement = nullptr;
  // auto storeStream =
  // this->getStream(inst->getTDG().stream_store().stream_id()); for (auto
  // element : this->userElementMap.at(inst->getSeqNum())) {
  //   if (element == nullptr) {
  //     continue;
  //   }
  //   if (element->stream == storeStream) {
  //     // Found it.
  //     storeElement = element;
  //     break;
  //   }
  // }
  // assert(storeElement != nullptr && "Failed to found the store element.");
  // callbacks.emplace_back(
  //     new StreamSQDeprecatedCallback(storeElement, inst));
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

void StreamEngine::cpuStoreTo(InstSeqNum seqNum, Addr vaddr, int size) {
  this->removePendingWritebackElement(seqNum, vaddr, size);
  if (this->numInflyStreamConfigurations == 0) {
    return;
  }
  if (auto element = this->peb.isHit(vaddr, size)) {
    // hack("CPU stores to (%#x, %d), hits in PEB.\n", vaddr, size);
    S_ELEMENT_DPRINTF_(StreamAlias, element, "CPUStoreTo aliased %#x, +%d.\n",
                       vaddr, size);
    // Remember that this element's address is aliased.
    element->isAddrAliased = true;
    this->flushPEB(vaddr, size);
  }
}

void StreamEngine::addPendingWritebackElement(StreamElement *releaseElement) {
  /**
   * ! This is a hack implementation to avoid alias for IndirectUpdateStream.
   * Ideally, we have to
   * 1. Either correctly flush all dependent elements when such alias happened.
   * 2. Or delay issuing when there is possible alias.
   *
   * This takes time to implement, and our current case is that alias only
   * happens within the same stream. So we take a hack approach:
   * 1. We delay issuing if there is any previous elements that alias.
   * 2. We remember all pending writeback elements and also search for it.
   *
   * We only need to search pending writeback elements for O3 CPU, as so far
   * our stream requests do not check O3 CPU's store buffer, and O3 CPU notifies
   * us cpuStoreTo() when it is sent to cache.
   *
   * While MinorCPU notifies us cpuStoreTo() when committing and moving into
   * StoreBuffer, and StreamRequests still check the store buffer, so we should
   * be fine.
   *
   * SimpleCPU always writeback before commit, so we are also fine.
   */
  if (this->cpuDelegator->cpuType != GemForgeCPUDelegator::O3) {
    S_ELEMENT_DPRINTF_(
        StreamAlias, releaseElement,
        "Skip PendingWritebackElements for Non-O3 CPU. sn:%llu.\n",
        releaseElement->firstStoreSeqNum);
  }
  S_ELEMENT_DPRINTF_(StreamAlias, releaseElement,
                     "Push into PendingWritebackElements. sn:%llu.\n",
                     releaseElement->firstStoreSeqNum);
  if (this->pendingWritebackElements.size() > 1000) {
    S_ELEMENT_PANIC(releaseElement, "Too many pending writeback elements.");
  }
  this->pendingWritebackElements.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(releaseElement->firstStoreSeqNum),
      std::forward_as_tuple(releaseElement->FIFOIdx, releaseElement->addr,
                            releaseElement->size));
}

void StreamEngine::removePendingWritebackElement(InstSeqNum seqNum, Addr vaddr,
                                                 int size) {
  /**
   * We assume writeback happens in order, and thus release any old elements.
   * This is because core may coalesce writes to the same address.
   */
  while (!this->pendingWritebackElements.empty()) {
    auto iter = this->pendingWritebackElements.begin();
    auto &element = iter->second;
    if (iter->first > seqNum) {
      break;
    }
    if (iter->first == seqNum) {
      SE_DPRINTF_(StreamAlias, "%s: Written back.\n", iter->second.fifoIdx);
      if (element.vaddr != vaddr || element.size != size) {
        DYN_S_PANIC(element.fifoIdx.streamId,
                    "Mismatch PendingWriteElement [%#x, +%d) vs [%#x, +%d).",
                    element.vaddr, element.size, vaddr, size);
      }
    } else {
      SE_DPRINTF_(StreamAlias, "%s: Assumed already written back.\n",
                  iter->second.fifoIdx);
    }
    this->pendingWritebackElements.erase(iter);
  }
}

bool StreamEngine::hasAliasWithPendingWritebackElements(
    StreamElement *checkElement, Addr vaddr, int size) const {
  for (const auto &pair : this->pendingWritebackElements) {
    const auto &element = pair.second;
    if (!(vaddr >= element.vaddr + element.size ||
          element.vaddr >= vaddr + size)) {
      S_ELEMENT_DPRINTF_(StreamAlias, checkElement,
                         "Access [%#x, +%d) aliased with "
                         "PendingWritebackElement %s [%#x, +%d).\n",
                         vaddr, size, element.fifoIdx, element.vaddr,
                         element.size);
      return true;
    }
  }
  return false;
}

void StreamEngine::tryInitializeStreams(
    const ::LLVM::TDG::StreamRegion &streamRegion) {
  for (const auto &streamInfo : streamRegion.streams()) {
    const auto &streamId = streamInfo.id();
    // Remember to also check the coalesced id map.
    if (this->streamMap.count(streamId) == 0 &&
        this->coalescedStreamIdMap.count(streamId) == 0) {
      // We haven't initialize streams in this loop.
      this->initializeStreams(streamRegion);
      break;
    }
  }
}

void StreamEngine::initializeStreams(
    const ::LLVM::TDG::StreamRegion &streamRegion) {

  Stream::StreamArguments args;
  args.cpu = cpu;
  args.cpuDelegator = cpuDelegator;
  args.se = this;
  args.maxSize = this->defaultRunAheadLength;
  args.streamRegion = &streamRegion;

  // Sanity check that we do not have too many streams.
  auto totalAliveStreams = this->enableCoalesce
                               ? streamRegion.total_alive_coalesced_streams()
                               : streamRegion.total_alive_streams();
  if (totalAliveStreams * this->defaultRunAheadLength >
      this->totalRunAheadLength) {
    // If there are too many streams, we reduce the maxSize.
    args.maxSize = this->totalRunAheadLength / totalAliveStreams;
    if (args.maxSize < 3) {
      panic("Too many streams %s TotalAliveStreams %d, FIFOSize %d.\n",
            streamRegion.region().c_str(), totalAliveStreams,
            this->totalRunAheadLength);
    }
  }
  SE_DPRINTF_(StreamThrottle,
              "[Throttle] Initialize MaxSize %d TotalAliveStreasm %d\n",
              args.maxSize, totalAliveStreams);

  this->generateCoalescedStreamIdMap(streamRegion, args);

  std::vector<Stream *> createdStreams;
  for (const auto &streamInfo : streamRegion.streams()) {
    const auto &streamId = streamInfo.id();
    // Set per stream field in stream args.
    args.staticId = streamId;
    args.name = streamInfo.name().c_str();

    // Check if this stream belongs to CoalescedStream.
    auto coalescedIter = this->coalescedStreamIdMap.find(streamId);
    assert(coalescedIter != this->coalescedStreamIdMap.end() &&
           "Every stream should be a coalesced stream.");
    auto coalescedStreamId = coalescedIter->second;
    auto coalescedStream = this->streamMap.at(coalescedStreamId);
    assert(coalescedStream && "Illegal type for CoalescedStream.");
    coalescedStream->addStreamInfo(streamInfo);
    // Don't forget to push into created streams.
    if (std::find(createdStreams.begin(), createdStreams.end(),
                  coalescedStream) == createdStreams.end()) {
      createdStreams.push_back(coalescedStream);
    }
  }

  /**
   * Remember to finalize the streams, and remember if it's a nest stream.
   */
  for (auto newStream : createdStreams) {
    newStream->finalize();
    if (streamRegion.is_nest()) {
      newStream->setNested();
    }
  }
  for (auto newStream : createdStreams) {
    newStream->postFinalize();
  }
  /**
   * ! Hack: Some stream has crazy large element size, e.g. vectorized
   * ! stream_memset, we should limit the maxSize for them to 2.
   * Notice that this doesn't work with throttling so far, but stream_memset
   * has only StoreStream (which is currently not throttled), this should
   * be fine.
   * Currently the threshold is 8 cache lines.
   */
  for (auto newStream : createdStreams) {
    size_t memElementSize = newStream->getMemElementSize();
    if (memElementSize >= cpuDelegator->cacheLineSize() * 8) {
      // For now I just update
      newStream->maxSize =
          std::min(newStream->maxSize, static_cast<size_t>(2ull));
    }
  }

  /**
   * Recursively initialize all nest streams.
   */
  this->regionController->initializeRegion(streamRegion);
  for (const auto nestRegionRelativePath :
       streamRegion.nest_region_relative_paths()) {
    const auto &nestStreamRegion =
        this->getStreamRegion(nestRegionRelativePath);

    this->tryInitializeStreams(nestStreamRegion);
  }

  /**
   * After all nest streams are initialized, we try to initialize any inner-loop
   * dependence for streams and LoopBound.
   */
  this->regionController->postNestInitializeRegion(streamRegion);
  for (auto newStream : createdStreams) {
    newStream->fixInnerLoopBaseStreams();
  }
}

void StreamEngine::generateCoalescedStreamIdMap(
    const ::LLVM::TDG::StreamRegion &streamRegion,
    Stream::StreamArguments &args) {

  /**
   * The compiler would provide coalesce info based on offset, however,
   * we would like to split large offset into multiple streams.
   * For example, accessing a 2-D array a[i] and a[i + cols].
   * The offset is cols * sizeof(element), which is a row. These two accesses
   * are not coalesced, as it would require very large element size.
   */
  std::list<std::vector<const ::LLVM::TDG::StreamInfo *>> coalescedGroup;
  // Collect coalesced info and reconstruct coalesced group.
  for (const auto &streamInfo : streamRegion.streams()) {
    const auto &coalesceInfo = streamInfo.coalesce_info();
    auto coalesceGroup = coalesceInfo.base_stream();
    [[maybe_unused]] constexpr uint64_t InvalidCoalesceGroup = 0;
    assert(coalesceGroup != InvalidCoalesceGroup && "Invalid CoalesceGroup.");
    // Search for the group. This is O(N^2).
    bool found = false;
    for (auto &group : coalescedGroup) {
      if (group.front()->coalesce_info().base_stream() == coalesceGroup) {
        group.push_back(&streamInfo);
        found = true;
        break;
      }
    }
    if (!found) {
      coalescedGroup.emplace_back();
      coalescedGroup.back().push_back(&streamInfo);
    }
  }
  // Sort each group with increasing order of offset.
  for (auto &group : coalescedGroup) {
    std::sort(group.begin(), group.end(),
              [](const ::LLVM::TDG::StreamInfo *a,
                 const ::LLVM::TDG::StreamInfo *b) -> bool {
                auto offsetA = a->coalesce_info().offset();
                auto offsetB = b->coalesce_info().offset();
                return offsetA < offsetB;
              });
  }
  // Resplit each group dependending on the expansion.
  for (auto groupIter = coalescedGroup.begin();
       groupIter != coalescedGroup.end(); ++groupIter) {
    if (groupIter->size() == 1) {
      // Ignore single streams.
      continue;
    }
    auto baseS = groupIter->front();
    auto baseOffset = baseS->coalesce_info().offset();
    auto endOffset = baseOffset;
    auto baseSEnabledStoreFunc =
        baseS->static_info().compute_info().enabled_store_func();
    size_t nStream = 0;
    for (auto streamIter = groupIter->begin(), streamEnd = groupIter->end();
         streamIter != streamEnd; ++streamIter, ++nStream) {
      const auto *streamInfo = *streamIter;
      auto offset = streamInfo->coalesce_info().offset();
      auto enabledStoreFunc =
          streamInfo->static_info().compute_info().enabled_store_func();
      if ((!this->enableCoalesce && nStream == 1) || offset > endOffset ||
          enabledStoreFunc != baseSEnabledStoreFunc) {
        /**
         * Split the group if one of the following happens:
         * 1. We explicitly disabled coalescing.
         * 2. The expansion is broken.
         * 3. The new stream has different enabledStoreFunc than the baseS.
         */
        assert(nStream != 0 && "Emplty LHS group.");
        coalescedGroup.emplace_back(streamIter, streamEnd);
        groupIter->resize(nStream);
        break;
      } else {
        // The expansion keeps going.
        endOffset = std::max(
            endOffset, offset + streamInfo->static_info().mem_element_size());
      }
    }
  }
  // For each split group, generate a Stream.
  for (auto &group : coalescedGroup) {
    Stream *stream = nullptr;
    uint64_t coalescedStreamId = 0;
    for (int i = 0; i < group.size(); ++i) {
      const auto &streamInfo = *(group[i]);
      const auto &streamId = streamInfo.id();
      args.staticId = streamId;
      args.name = streamInfo.name().c_str();
      if (i == 0) {
        // The first stream is the leading stream.
        /**
         * I know this is confusing. But there are two possible interpretation
         * of coalesce group: trace-based ane execution-based.
         * Both version uses coalesce base stream id as the coalesce group.
         * 1. In the old trace based implementation, coalesced stream will have
         *    offset -1 (we don't calculate the offset in transformation).
         * 2. In the static transform implementation, the offset should be >= 0.
         * NOTE: Now we always use new execution-based coalescing, even in trace
         * simulation.
         */

        // TODO: I don't like this design, all initialization should happen
        // TODO: in the function initializeStreams().
        stream = new Stream(args);
        coalescedStreamId = streamId;
        this->streamMap.emplace(coalescedStreamId, stream);
      }
      this->coalescedStreamIdMap.emplace(streamId, coalescedStreamId);
    }
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

Stream *StreamEngine::getStream(const std::string &streamName) const {
  for (auto &entry : this->streamMap) {
    auto S = entry.second;
    if (S->getStreamName() == streamName) {
      return S;
    }
  }
  panic("Failed to find stream %s.\n", streamName);
}

Stream *StreamEngine::tryGetStream(uint64_t streamId) const {
  if (this->coalescedStreamIdMap.count(streamId)) {
    streamId = this->coalescedStreamIdMap.at(streamId);
  }
  auto iter = this->streamMap.find(streamId);
  if (iter == this->streamMap.end()) {
    return nullptr;
  }
  return iter->second;
}

void StreamEngine::tick() {
  this->regionController->tick();
  this->issueElements();
  this->computeEngine->startComputation();
  this->computeEngine->completeComputation();
  this->floatController->processMidwayFloat();
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
    if (!stream->isConfigured()) {
      continue;
    }
    if (stream->isMemStream()) {
      totalAliveMemStreams++;
    }
    stream->sampleStatistic();
  }
  this->numTotalAliveElements.sample(totalAliveElements);
  this->numTotalAliveCacheBlocks.sample(totalAliveCacheBlocks.size());
  this->numTotalAliveMemStreams.sample(totalAliveMemStreams);
  // Update infly StreamRequests distribution.
  this->numInflyStreamRequestDist.sample(this->numInflyStreamRequests);
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

void StreamEngine::addFreeElement(StreamElement *elem) {
  elem->clearInflyMemAccesses();
  elem->clear();
  elem->next = this->FIFOFreeListHead;
  this->FIFOFreeListHead = elem;
  this->numFreeFIFOEntries++;
}

StreamElement *StreamEngine::removeFreeElement() {
  assert(this->hasFreeElement() && "No free element to remove.");
  auto newElem = this->FIFOFreeListHead;
  this->FIFOFreeListHead = this->FIFOFreeListHead->next;
  this->numFreeFIFOEntries--;
  newElem->clear();
  return newElem;
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
  /**
   * Create the list by topological sort.
   */
  std::list<Stream *> stepList;
  std::list<Stream *> stack;
  std::unordered_map<Stream *, int> stackStatusMap;

  auto pushToStack = [&stack, &stackStatusMap](Stream *S) -> void {
    auto status = stackStatusMap.emplace(S, 0).first->second;
    if (status == 1) {
      // Cycle dependence found.
      panic("Cycle dependence found %s.", S->getStreamName());
    } else if (status == 2) {
      // This one has already dumped.
      return;
    } else {
      // This one has not been visited.
      stack.emplace_back(S);
    }
  };

  stack.emplace_back(stepS);
  stackStatusMap.emplace(stepS, 0);
  while (!stack.empty()) {
    auto S = stack.back();
    if (stackStatusMap.at(S) == 0) {
      // First time.
      for (auto depS : S->addrDepStreams) {
        if (depS->getLoopLevel() != stepS->getLoopLevel()) {
          continue;
        }
        pushToStack(depS);
      }
      // Value dep stream.
      for (auto depS : S->valueDepStreams) {
        if (depS->getLoopLevel() != stepS->getLoopLevel()) {
          continue;
        }
        pushToStack(depS);
      }
      /**
       * Also respect the merged predicated relationship.
       */
      for (auto predStreamId : S->getMergedPredicatedStreams()) {
        auto predS = this->getStream(predStreamId.id().id());
        assert(predS->stepRootStream == stepS &&
               "PredicatedStream should have same step root.");
        pushToStack(predS);
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

const std::list<Stream *> &StreamEngine::getConfigStreamsInRegion(
    const LLVM::TDG::StreamRegion &streamRegion) {
  if (this->memorizedRegionConfiguredStreamsMap.count(&streamRegion)) {
    return this->memorizedRegionConfiguredStreamsMap.at(&streamRegion);
  }
  /**
   * Get all the configured streams.
   */
  auto &configStreams = this->memorizedRegionConfiguredStreamsMap
                            .emplace(std::piecewise_construct,
                                     std::forward_as_tuple(&streamRegion),
                                     std::forward_as_tuple())
                            .first->second;
  std::unordered_set<Stream *> dedupSet;
  for (const auto &streamInfo : streamRegion.streams()) {
    // Deduplicate the streams due to coalescing.
    const auto &streamId = streamInfo.id();
    auto stream = this->getStream(streamId);
    if (dedupSet.count(stream) == 0) {
      // We insert the whole StepStreams to reuse the topological sort result.
      if (stream->stepRootStream == stream) {
        const auto &stepStreams = this->getStepStreamList(stream);
        configStreams.insert(configStreams.end(), stepStreams.begin(),
                             stepStreams.end());
        dedupSet.insert(stepStreams.begin(), stepStreams.end());
      }
    }
  }
  return configStreams;
}

void StreamEngine::allocateElement(DynStream &dynS) {
  auto newElement = this->removeFreeElement();
  this->numElementsAllocated++;
  auto S = dynS.stream;
  if (S->getStreamType() == ::LLVM::TDG::StreamInfo_Type_LD) {
    this->numLoadElementsAllocated++;
  } else if (S->getStreamType() == ::LLVM::TDG::StreamInfo_Type_ST) {
    this->numStoreElementsAllocated++;
  }

  dynS.allocateElement(newElement);

  // Add the DynS to IssueList.
  this->addIssuingDynS(&dynS);
}

void StreamEngine::releaseElementStepped(DynStream *dynS, bool isEnd,
                                         bool doThrottle) {

  /**
   * This function performs a normal release, i.e. release a stepped
   * element.
   */
  auto S = dynS->stream;

  auto releaseElement = dynS->releaseElementStepped(isEnd);
  /**
   * How to handle short streams?
   * There is a pathological case when the streams are short, and
   * increasing the run ahead length beyond the stream length does not
   * make sense. We do not throttle if the element is within the run ahead
   * length.
   */
  if (doThrottle) {
    this->throttler->throttleStream(releaseElement);
  }

  const bool used = releaseElement->isFirstUserDispatched();
  if (releaseElement->isLastElement() && !S->isInnerFinalValueUsedByCore()) {
    assert(!used && "LastElement of NonReductionStream released being used.");
  }

  /**
   * Sanity check that all the user are done with this element.
   */
  if (this->elementUserMap.count(releaseElement) != 0) {
    assert(this->elementUserMap.at(releaseElement).empty() &&
           "Some unreleased user instruction.");
  }

  if (S->isLoadStream()) {
    this->numLoadElementsStepped++;
    if (used) {
      this->numLoadElementsUsed++;
      // Update waited cycle information.
      auto waitedCycles = 0;
      if (releaseElement->valueReadyCycle >
          releaseElement->firstValueCheckCycle) {
        waitedCycles = releaseElement->valueReadyCycle -
                       releaseElement->firstValueCheckCycle;
      }
      this->numLoadElementWaitCycles += waitedCycles;
    }
  } else if (S->isStoreStream()) {
    this->numStoreElementsStepped++;
    if (used) {
      this->numStoreElementsUsed++;
    }
  }

  /**
   * For a issued element, if unused, it should be removed from PEB.
   */
  if (S->trackedByPEB() && releaseElement->isReqIssued()) {
    if (used) {
      if (this->peb.contains(releaseElement)) {
        S_ELEMENT_PANIC(releaseElement,
                        "Used element still in PEB when released.");
      }
    } else {
      this->peb.removeElement(releaseElement);
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
        assert(cacheBlockInfo.pendingAccesses.empty() &&
               "Merge data for streams is removed.");
        if (cacheBlockInfo.used && cacheBlockInfo.requestedByLoad) {
          this->numLoadCacheLineUsed++;
        }
        this->cacheBlockRefMap.erase(cacheBlockVAddr);
      }
    }
  }

  this->addFreeElement(releaseElement);
}

bool StreamEngine::releaseElementUnstepped(DynStream &dynS) {
  auto S = dynS.stream;
  auto elem = dynS.releaseElementUnstepped();
  if (elem) {
    if (S->trackedByPEB() && elem->isReqIssued()) {
      // This should be in PEB.
      this->peb.removeElement(elem);
    }
    this->addFreeElement(elem);
  }
  return elem != nullptr;
}

void StreamEngine::addIssuingDynS(DynStream *dynS) {
  DYN_S_DPRINTF(dynS->dynStreamId, "Try Add to IssueList: Already In? %d.\n",
                this->issuingDynStreamSet.count(dynS));
  this->issuingDynStreamSet.insert(dynS);
}

StreamEngine::DynStreamSet::iterator
StreamEngine::removeIssuingDynS(DynStreamSet::iterator iter) {
  DYN_S_DPRINTF((*iter)->dynStreamId, "Remove from IssueList.\n");
  return this->issuingDynStreamSet.erase(iter);
}

void StreamEngine::removeIssuingDynS(DynStream *dynS) {
  DYN_S_DPRINTF(dynS->dynStreamId, "Try Remove from IssueList: %d.\n",
                this->issuingDynStreamSet.count(dynS));
  this->issuingDynStreamSet.erase(dynS);
}

std::vector<StreamElement *> StreamEngine::findReadyElements() {
  std::vector<StreamElement *> readyElems;

  /**
   * We iterate through all configured streams' elements.
   * In this way, elements are marked ready in order, i.e. if
   * one element is not ready then we break searching in this stream.
   */
  for (auto iter = this->issuingDynStreamSet.begin(),
            end = this->issuingDynStreamSet.end();
       iter != end;) {
    auto &dynS = **iter;
    auto S = dynS.stream;

    bool hasUnissuedElem = false;

    DYN_S_DPRINTF(dynS.dynStreamId, "Try Issue. AllocSize %d.\n",
                  dynS.allocSize);

    for (auto elem = dynS.tail->next; elem; elem = elem->next) {

      if (elem->isElemFloatedToCache() && dynS.isFloatConfigDelayed()) {
        S_ELEMENT_DPRINTF(elem, "NotReady as FloatConfigDelayed.\n");
        hasUnissuedElem = true;
        break;
      }

      /**
       * There three types of ready elements:
       * 1. All the address base elements are ready -> compute address.
       *  This represents all Mem streams.
       * 2. Address is ready, value base elements are ready -> compute value.
       *  This applies to IV/Reduction/StoreCompute/LoadCompute/Update
       * streams.
       *  NOTE: With one exception: InCore LoopElim StoreComputeS is first
       * ValReady then issued. See below.
       * 3. Address is ready, request not issued, first user is
       * non-speculative
       *  -> Issue the atomic request.
       *  This applies to AtomicCompute streams.
       *
       * New issue logic for LoopElimInCoreStoreCmpElem.
       * 1. They are first markedAddrReady but not issued. This is handled
       * below.
       * 2. They are scheduled the computation. This is handled above.
       * 3. When both addrReady() and computeValueReady(), we finally issue
       * them.
       */
      if (elem->isAddrReady()) {
        // Address already ready. Check if we have type 2 or 3 ready elements.
        if (elem->shouldComputeValue() && !elem->scheduledComputation &&
            !elem->isComputeValueReady()) {
          hasUnissuedElem = true;
          if (elem->checkValueBaseElemsValueReady()) {
            S_ELEMENT_DPRINTF(elem, "Found Ready for Compute.\n");
            readyElems.emplace_back(elem);
          }
        }
        if (S->isAtomicComputeStream() && !elem->isElemFloatedToCache() &&
            !elem->isReqIssued()) {
          // Check that StreamAtomic inst is non-speculative, i.e. it checks
          // if my value is ready.
          hasUnissuedElem = true;
          if (elem->firstValueCheckByCoreCycle != 0) {
            S_ELEMENT_DPRINTF(
                elem, "StreamAtomic is non-speculative, ready to issue.\n");
            readyElems.emplace_back(elem);
          }
        }
        if (elem->isLoopElimInCoreStoreCmpElem() && !elem->isReqIssued() &&
            elem->isComputeValueReady()) {
          hasUnissuedElem = true;
          S_ELEMENT_DPRINTF(
              elem,
              "LoopElimInCoreStoreS is Addr/ValueReady. Ready to issue.\n");
          readyElems.emplace_back(elem);
        }
        continue;
      }
      /**
       * To avoid overhead, if an element is aliased, we do not try to
       * issue it until the first user is dispatched. However, now we
       * have removed the load b[i] for indirect access like a[b[i]],
       * we need to further check if my dependent stream has user dispatched
       * to avoid deadlock.
       *
       * TODO: This adhoc fix only works for one level, if it's a[b[c]] then
       * TODO: it will deadlock again.
       * TODO: Record dependent element information to avoid this expensive
       * TODO: search.
       */
      if (elem->isAddrAliased && !elem->isFirstUserDispatched()) {
        bool hasIndirectUserDispatched = false;
        for (auto depS : S->addrDepStreams) {
          auto &dynDepS = depS->getDynStream(dynS.configSeqNum);
          auto depElement = dynDepS.getElemByIdx(elem->FIFOIdx.entryIdx);
          if (depElement && depElement->isFirstUserDispatched()) {
            hasIndirectUserDispatched = true;
          }
        }
        if (!hasIndirectUserDispatched) {
          hasUnissuedElem = true;
          break;
        }
      }

      if (S->isDelayIssueUntilFIFOHead()) {
        if (elem != dynS.tail->next) {
          S_ELEMENT_DPRINTF(elem, "[NotReady] Not FIFO Head.\n");
          hasUnissuedElem = true;
          break;
        }
      }

      /**
       * I noticed that sometimes we have prefetched too early for
       * AtomicStream, especially for gap.bfs_push. The line we prefetched may
       * be evicted before the core issues the request and we still got
       * coherence miss. This may cause some extra traffix overhead in the
       * NoC. Here I try to limit the prefetch distance for AtomicStream
       * without computation.
       */
      if (S->isAtomicStream() && !elem->isElemFloatedToCache()) {
        hasUnissuedElem = true;
        if (elem->FIFOIdx.entryIdx >
            dynS.getFirstElem()->FIFOIdx.entryIdx +
                this->myParams->maxNumElementsPrefetchForAtomic) {
          break;
        }
      }

      /**
       * Should not issue.
       */
      if (!elem->shouldIssue()) {
        S_ELEMENT_DPRINTF(elem, "Not Issue. Continue.\n");
        continue;
      }
      auto baseElemsValReady =
          elem->checkAddrBaseElementsReady(false /* CheckByCore */);
      auto canNDCIssue = this->ndcController->canIssueNDCPacket(elem);
      if (baseElemsValReady && canNDCIssue) {
        S_ELEMENT_DPRINTF(elem, "Found Addr Ready.\n");
        readyElems.emplace_back(elem);
      } else {
        // We should not check the next one as we should issue inorder.
        if (!baseElemsValReady) {
          S_ELEMENT_DPRINTF(elem, "Not Addr Ready. Break.\n");
        } else {
          S_ELEMENT_DPRINTF(elem, "Not NDC Ready. Break.\n");
        }
        hasUnissuedElem = true;
        break;
      }
    }

    if (!hasUnissuedElem) {
      iter = this->removeIssuingDynS(iter);
    } else {
      ++iter;
    }
  }

  return readyElems;
}

void StreamEngine::issueElements() {
  // Find all ready elements.
  auto readyElements = this->findReadyElements();

  /**
   * Sort the ready elements by create cycle and relative order within
   * the single stream.
   */
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
  for (auto &elem : readyElements) {

    auto S = elem->stream;

    if (elem->isAddrReady() && S->shouldComputeValue() &&
        !elem->isComputeValueReady()) {
      // Type 2 ready elements.
      assert(!elem->scheduledComputation);
      elem->computeValue();
      continue;
    }

    /**
     * Sanity check: for loop eliminated AtomicComputeS,
     * we have to offload.
     */
    if (S->isLoopEliminated() && S->isAtomicComputeStream()) {
      if (!elem->isElemFloatedToCache()) {
        S_ELEMENT_PANIC(elem,
                        "LoopElim AtomicComputeS can not execute in core.");
      }
    }

    if (!elem->isAddrReady()) {
      /**
       * For IndirectUpdateStream, we have possible aliasing, and for now
       * we cannot correctly track all the dependence and flush/rewind.
       * To avoid that, here I check if any previous element aliased with
       * the issuing element. If found, we do not issue.
       *
       * ! This may greatly limit the prefetch distance. For now we disable
       * ! this check.
       */
      elem->markAddrReady();
    }

    if (S->isMemStream()) {
      assert(!elem->isReqIssued() && "Element already issued request.");

      /**
       * * New Feature: If the stream is merged, then we do not issue.
       * * The stream should never be used by the core.
       */
      if (S->isMerged()) {
        continue;
      }

      if (!elem->shouldIssue()) {
        continue;
      }

      /**
       * AtomicComputeStream will mark the AddrReady, but delay issuing
       * until the StreamAtomic instruction is non-speculative, which
       * means it started to check if element value is ready for writeback.
       * So we check the firstValueCheckByCoreCycle.
       * If the stream is floated, then we can immediately issue.
       */
      if (S->isAtomicStream() && !elem->isElemFloatedToCache() &&
          !elem->isElemFloatedAsNDC()) {
        if (!elem->isPrefetchIssued()) {
          // We first issue prefetch request for AtomicStream.
          this->prefetchElement(elem);
          continue;
        }
        if (S->isAtomicComputeStream() &&
            elem->firstValueCheckByCoreCycle == 0) {
          S_ELEMENT_DPRINTF(
              elem, "Delay issue as waiting for FirstValueCheckByCore.\n");
          continue;
        }
      }

      if (elem->isLoopElimInCoreStoreCmpElem()) {
        if (!elem->isComputeValueReady()) {
          S_ELEMENT_DPRINTF(elem, "Delay issue as waiting for StoreValue.\n");
        } else {
          this->writebackElement(elem);
        }
        continue;
      }

      /**
       * Intercept the NDC request.
       */
      if (elem->isElemFloatedAsNDC()) {
        this->issueNDCElement(elem);
        continue;
      }

      // Increase the reference of the cache block if we enable merging.
      if (this->enableMerge) {
        for (int i = 0; i < elem->cacheBlocks; ++i) {
          auto cacheBlockAddr =
              elem->cacheBlockBreakdownAccesses[i].cacheBlockVAddr;
          this->cacheBlockRefMap
              .emplace(std::piecewise_construct,
                       std::forward_as_tuple(cacheBlockAddr),
                       std::forward_as_tuple())
              .first->second.reference++;
        }
      }
      // Issue the element.
      this->issueElement(elem);
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
#ifndef NDEBUG
  for (auto &pendingMemAccess : cacheBlockInfo.pendingAccesses) {
    assert(pendingMemAccess != memAccess &&
           "pendingMemAccess should not be fetching access.");
    assert(false && "Merge data for streams is removed.");
  }
#endif
  // Remember to clear the pendingAccesses, as they are now released.
  cacheBlockInfo.pendingAccesses.clear();
}

void StreamEngine::issueElement(StreamElement *elem) {
  assert(elem->isAddrReady() && "Address should be ready.");
  assert(elem->stream->isMemStream() &&
         "Should never issue element for IVStream.");
  assert(elem->shouldIssue() && "Should not issue this element.");
  assert(!elem->isReqIssued() && "Element req already issued.");

  auto S = elem->stream;
  auto dynS = elem->dynS;
  if (elem->flushed) {
    if (!S->trackedByPEB()) {
      S_ELEMENT_PANIC(elem, "Flushed Non-PEB stream element.");
    }
    S_ELEMENT_DPRINTF(elem, "Issue - Reissue.\n");
  } else {
    S_ELEMENT_DPRINTF(elem, "Issue.\n");
  }
  if (S->isLoadStream()) {
    this->numLoadElementsFetched++;
  }
  S->statistic.numFetched++;
  elem->setReqIssued();
  if (S->trackedByPEB() && !elem->isFirstUserDispatched()) {
    // Add to the PEB if the first user has not been dispatched.
    this->peb.addElement(elem);
  }

  /**
   * A quick hack to coalesce continuous elements that completely overlap.
   */
  this->coalesceContinuousDirectMemStreamElement(elem);

  for (size_t i = 0; i < elem->cacheBlocks; ++i) {
    auto &cacheBlockBreakdown = elem->cacheBlockBreakdownAccesses[i];

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

    const auto cacheLineVAddr = cacheBlockBreakdown.cacheBlockVAddr;
    const auto cacheLineSize = cpuDelegator->cacheLineSize();
    if ((cacheLineVAddr % cacheLineSize) != 0) {
      S_ELEMENT_PANIC(elem, "CacheBlock %d LineVAddr %#x invalid, VAddr %#x.\n",
                      i, cacheLineVAddr, cacheBlockBreakdown.vaddr);
    }
    Addr cacheLinePAddr;
    if (!cpuDelegator->translateVAddrOracle(cacheLineVAddr, cacheLinePAddr)) {
      S_ELEMENT_DPRINTF(elem, "Fault on vaddr %#x.\n", cacheLineVAddr);
      cacheBlockBreakdown.state = CacheBlockBreakdownAccess::StateE::Faulted;
      /**
       * The current mechanism to mark value ready is too hacky.
       * We rely on the setValue() to call tryMarkValueReady().
       * However, since Faulted is also considered ready, we have to
       * call tryMarkValueReady() whenver we set a block to Faulted state.
       * TODO: Improve this poor design.
       */
      elem->tryMarkValueReady();
      continue;
    }
    if ((cacheLinePAddr % cacheLineSize) != 0) {
      S_ELEMENT_PANIC(
          elem, "LinePAddr %#x invalid, LineVAddr %#x, VAddr %#x.\n",
          cacheLinePAddr, cacheLineVAddr, cacheBlockBreakdown.vaddr);
    }

    /**
     * Some special case for ReqFlags:
     * 1. For offloaded streams, they rely on this request to advance in
     * MLC. Disable RubySequencer coalescing for that. Unless this is a
     * reissue request, which should be treated normally.
     * 2. For Store/Atomic stream without computation, issue this as ReadEx
     * request to be prefetched in Exclusive state. These streams will never
     * be offloaded to cache, but we check just to be sure.
     * 3. For offloaded AtomicComputeStream and LoadComputeStream, the value
     * is computed in the LLC, and we set NO_RUBY_BACK_STORE to prevent the
     * Sequencer overwrite the result.
     * 4. For LoadStream with Update but not promoted into UpdateStreams, we
     * issue this as ReadEx as the StoreStream is not configured.
     */
    Request::Flags flags;
    if (elem->isElemFloatedToCache()) {
      if (!elem->flushed) {
        flags.set(Request::NO_RUBY_SEQUENCER_COALESCE);
      }
      if (S->isAtomicComputeStream() || S->isLoadComputeStream() ||
          S->isUpdateStream()) {
        if (elem->flushed) {
          S_ELEMENT_PANIC(
              elem, "Flushed Floating Atomic/LoadCompute/UpdateStream.\n");
        }
        flags.set(Request::NO_RUBY_BACK_STORE);
      }
    }
    if (S->isStoreStream() || S->isAtomicStream()) {
      if (!S->isStoreComputeStream() && !S->isAtomicComputeStream()) {
        if (!elem->isElemFloatedToCache()) {
          flags.set(Request::READ_EXCLUSIVE);
        }
      }
    }
    if (S->isLoadStream() && S->hasUpdate() && !elem->isElemFloatedToCache()) {
      flags.set(Request::READ_EXCLUSIVE);
    }

    // Allocate the book-keeping StreamMemAccess.
    auto memAccess = elem->allocateStreamMemAccess(cacheBlockBreakdown);
    PacketPtr pkt = nullptr;
    if (S->isAtomicComputeStream()) {
      if (elem->cacheBlocks != 1) {
        S_ELEMENT_PANIC(elem, "Illegal # of CacheBlocks %d for AtomicOp.",
                        elem->cacheBlocks);
      }
      /**
       * It is the programmer/compiler's job to make sure no aliasing for
       * computation (i.e. StoreFunc), so the element should never be flushed.
       */
      if (elem->flushed) {
        S_ELEMENT_PANIC(elem,
                        "AtomicStream with StoreFunc should not be flushed.");
      }
      if (elem->isElemFloatedToCache()) {
        // Offloaded the whole stream.
        pkt = GemForgePacketHandler::createGemForgePacket(
            cacheLinePAddr, cacheLineSize, memAccess, nullptr /* Data */,
            cpuDelegator->dataRequestorId(), 0 /* ContextId */,
            S->getFirstCoreUserPC() /* PC */, flags);
        pkt->req->setVirt(cacheLineVAddr);
      } else {
        // Special case to handle computation for atomic stream at CoreSE.
        auto getBaseValue = [elem](Stream::StaticId id) -> StreamValue {
          return elem->getValueBaseByStreamId(id);
        };
        auto atomicOp = S->setupAtomicOp(elem->FIFOIdx, elem->size,
                                         dynS->storeFormalParams, getBaseValue);
        // * We should use element address here, not line address.
        auto elementVAddr = cacheBlockBreakdown.vaddr;
        auto lineOffset = elementVAddr % cacheLineSize;
        auto elementPAddr = cacheLinePAddr + lineOffset;

        // Atomic is also considered as computation stats.
        S->recordComputationInCoreStats();
        this->numScheduledComputation++;

        this->computeEngine->recordCompletedStats(S);

        pkt = GemForgePacketHandler::createGemForgeAMOPacket(
            elementVAddr, elementPAddr, elem->size, memAccess,
            cpuDelegator->dataRequestorId(), 0 /* ContextId */,
            S->getFirstCoreUserPC() /* PC */, std::move(atomicOp));
      }
    } else {
      pkt = GemForgePacketHandler::createGemForgePacket(
          cacheLinePAddr, cacheLineSize, memAccess, nullptr /* Data */,
          cpuDelegator->dataRequestorId(), 0 /* ContextId */,
          S->getFirstCoreUserPC() /* PC */, flags);
      pkt->req->setVirt(cacheLineVAddr);
    }
    pkt->req->getStatistic()->isStream = true;
    pkt->req->getStatistic()->streamName = S->streamName.c_str();
    S_ELEMENT_DPRINTF(elem, "Issued %dth request to %#x %d.\n", i,
                      pkt->getAddr(), pkt->getSize());

    {
      // Sanity check that no multi-line element.
      auto lineOffset = pkt->getAddr() % cacheLineSize;
      if (lineOffset + pkt->getSize() > cacheLineSize) {
        S_ELEMENT_PANIC(elem,
                        "Issued Multi-Line request to %#x size %d, "
                        "lineVAddr %#x linePAddr %#x.",
                        pkt->getAddr(), pkt->getSize(), cacheLineVAddr,
                        cacheLinePAddr);
      }
    }
    S->statistic.numIssuedRequest++;
    if (flags.isSet(Request::READ_EXCLUSIVE)) {
      S->statistic.numIssuedReadExRequest++;
    }
    elem->dynS->incrementNumIssuedRequests();
    S->incrementInflyStreamRequest();
    this->incrementInflyStreamRequest();

    // Mark the state.
    cacheBlockBreakdown.state = CacheBlockBreakdownAccess::StateE::Issued;
    cacheBlockBreakdown.memAccess = memAccess;
    memAccess->registerReceiver(elem);

    if (cpuDelegator->cpuType == GemForgeCPUDelegator::ATOMIC_SIMPLE) {
      // Directly send to memory for atomic cpu.
      this->cpuDelegator->sendRequest(pkt);
    } else {
      // Send the pkt to translation.
      this->translationBuffer->addTranslation(
          pkt, cpuDelegator->getSingleThreadContext(), nullptr);
    }
  }
}

void StreamEngine::issueNDCElement(StreamElement *element) {
  assert(element->isAddrReady() && "Address should be ready.");
  assert(element->stream->isMemStream() &&
         "Should never issue element for IVStream.");
  assert(element->shouldIssue() && "Should not issue this element.");
  assert(!element->isReqIssued() && "Element req already issued.");
  assert(!element->flushed && "Flushed NDC element.");

  auto S = element->stream;
  S->statistic.numNDCed++;
  element->setReqIssued();
  if (S->trackedByPEB() && !element->isFirstUserDispatched()) {
    // Add to the PEB if the first user has not been dispatched.
    this->peb.addElement(element);
  }

  /**
   * NDC is performed at element granularity, so no coalescing.
   */
  this->ndcController->issueNDCPacket(element);

  for (size_t i = 0; i < element->cacheBlocks; ++i) {
    auto &cacheBlockBreakdown = element->cacheBlockBreakdownAccesses[i];

    // Check if this cache line is already done.
    assert(cacheBlockBreakdown.state ==
           CacheBlockBreakdownAccess::StateE::Initialized);

    const auto cacheLineVAddr = cacheBlockBreakdown.cacheBlockVAddr;
    const auto cacheLineSize = cpuDelegator->cacheLineSize();
    if ((cacheLineVAddr % cacheLineSize) != 0) {
      S_ELEMENT_PANIC(element,
                      "CacheBlock %d LineVAddr %#x invalid, VAddr %#x.\n", i,
                      cacheLineVAddr, cacheBlockBreakdown.vaddr);
    }
    Addr cacheLinePAddr;
    if (!cpuDelegator->translateVAddrOracle(cacheLineVAddr, cacheLinePAddr)) {
      S_ELEMENT_PANIC(element, "Fault on NDC vaddr %#x.\n", cacheLineVAddr);
    }

    // Mark the state.
    cacheBlockBreakdown.state = CacheBlockBreakdownAccess::StateE::Issued;
  }
}

void StreamEngine::prefetchElement(StreamElement *elem) {
  auto S = elem->stream;
  auto dynS = elem->dynS;
  S_ELEMENT_DPRINTF(elem, "Prefetch.\n");

  assert(elem->isAddrReady() && "Address should be ready for prefetch.");
  assert(S->isAtomicStream() && "So far we only prefetch for AtomicStream.");
  assert(elem->shouldIssue() && "Should not prefetch this element.");
  assert(!elem->isPrefetchIssued() && "Element prefetch already issued.");
  assert(!elem->isElemFloatedToCache() &&
         "Should not prefetch for floating stream.");

  S->statistic.numPrefetched++;
  elem->setPrefetchIssued();

  for (size_t i = 0; i < elem->cacheBlocks; ++i) {
    // Prefetch the whole cache line.
    auto &cacheBlockBreakdown = elem->cacheBlockBreakdownAccesses[i];
    const auto cacheLineVAddr = cacheBlockBreakdown.cacheBlockVAddr;
    const auto cacheLineSize = cpuDelegator->cacheLineSize();
    if ((cacheLineVAddr % cacheLineSize) != 0) {
      S_ELEMENT_PANIC(elem, "CacheBlock %d LineVAddr %#x invalid, VAddr %#x.\n",
                      i, cacheLineVAddr, cacheBlockBreakdown.vaddr);
    }

    /**
     * Skip prefetching if we found a previous element and it has the same
     * block.
     */
    if (elem != dynS->getFirstElem()) {
      auto prevElement = dynS->getPrevElement(elem);
      bool prefetched = false;
      for (auto j = 0; j < prevElement->cacheBlocks; ++j) {
        const auto &prevCacheBlockBreakdown =
            prevElement->cacheBlockBreakdownAccesses[j];
        if (prevCacheBlockBreakdown.cacheBlockVAddr == cacheLineVAddr) {
          prefetched = true;
          break;
        }
      }
      if (prefetched) {
        // Already prefetched by previous element. Skip this block.
        continue;
      }
    }

    Addr cacheLinePAddr;
    if (!cpuDelegator->translateVAddrOracle(cacheLineVAddr, cacheLinePAddr)) {
      // If faulted, we just give up on this block.
      S_ELEMENT_DPRINTF(elem, "Fault on prefetch vaddr %#x.\n", cacheLineVAddr);
      continue;
    }

    PacketPtr pkt = nullptr;
    /**
     * Some special case for ReqFlags:
     * 1. For Store/Atomic stream, issue this prefetch as ReadEx
     * request to be prefetched in Exclusive state.
     */
    Request::Flags flags;
    if (S->isStoreStream() || S->isAtomicStream()) {
      flags.set(Request::READ_EXCLUSIVE);
    }

    /**
     * Since we don't care about the response for prefetch request,
     * here we use a dummy GemForgePacketReleaseHandler instead of normal
     * StreamMemAccess.
     */
    auto packetHandler = GemForgePacketReleaseHandler::get();
    pkt = GemForgePacketHandler::createGemForgePacket(
        cacheLinePAddr, cacheLineSize, packetHandler, nullptr /* Data */,
        cpuDelegator->dataRequestorId(), 0 /* ContextId */,
        S->getFirstCoreUserPC() /* PC */, flags);
    pkt->req->setVirt(cacheLineVAddr);
    pkt->req->getStatistic()->isStream = true;
    pkt->req->getStatistic()->streamName = S->streamName.c_str();
    S_ELEMENT_DPRINTF(elem, "Prefetched %dth request to %#x %d.\n", i,
                      pkt->getAddr(), pkt->getSize());

    S->statistic.numIssuedPrefetchRequest++;

    if (cpuDelegator->cpuType == GemForgeCPUDelegator::ATOMIC_SIMPLE) {
      // Directly send to memory for atomic cpu.
      this->cpuDelegator->sendRequest(pkt);
    } else {
      // Send the pkt to translation.
      this->translationBuffer->addTranslation(
          pkt, cpuDelegator->getSingleThreadContext(), nullptr);
    }
  }
}

void StreamEngine::writebackElement(StreamElement *elem) {

  auto S = elem->stream;
  S_ELEMENT_DPRINTF(elem, "Writeback.\n");

  assert(elem->isAddrReady() && "Address should be ready to writeback.");
  assert(elem->dynS->isLoopElimInCoreStoreCmpS() &&
         "So far only writeback for InCoreStoreCmpS.");
  assert(elem->shouldIssue() && "Should not writeback this element.");
  assert(!elem->isElemFloatedToCache() &&
         "Should not writeback floated element.");
  assert(!elem->isReqIssued() && "Element req already issued.");
  assert(!elem->flushed && "Cannot writeback flushed element.");

  S->statistic.numFetched++;
  elem->setReqIssued();

  for (size_t i = 0; i < elem->cacheBlocks; ++i) {
    // Only writeback the required data.
    auto &cacheBlockBreakdown = elem->cacheBlockBreakdownAccesses[i];
    const auto cacheLineVAddr = cacheBlockBreakdown.cacheBlockVAddr;
    const auto vaddr = cacheBlockBreakdown.vaddr;
    const auto size = cacheBlockBreakdown.size;

    const auto value = elem->getValuePtr(vaddr, size);
    S_ELEMENT_DPRINTF(
        elem, "[Writeback] CacheBlock %d Line %#x Actual [%#x, +%d) %s.\n", i,
        cacheLineVAddr, vaddr, size, GemForgeUtils::dataToString(value, size));

    Addr paddr;
    if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
      S_ELEMENT_PANIC(elem, "Fault on writeback vaddr %#x.\n", vaddr);
      continue;
    }

    /**
     * Here we don't actually wait for the response, so we use a dummy
     * GemForgePacketReleaseHandler instead of normal StreamMemAccess.
     * TODO: Use real StreamMemAccess and delay release until all store
     * TODO: requests are done.
     */
    Request::Flags flags;
    auto packetHandler = GemForgePacketReleaseHandler::get();
    PacketPtr pkt = GemForgePacketHandler::createGemForgePacket(
        paddr, size, packetHandler, value, cpuDelegator->dataRequestorId(),
        0 /* ContextId */, S->getFirstCoreUserPC() /* PC */, flags);
    pkt->req->setVirt(vaddr);
    pkt->req->getStatistic()->isStream = true;
    pkt->req->getStatistic()->streamName = S->streamName.c_str();

    if (cpuDelegator->cpuType == GemForgeCPUDelegator::ATOMIC_SIMPLE) {
      // Directly send to memory for atomic cpu.
      this->cpuDelegator->sendRequest(pkt);
    } else {
      // Send the pkt to translation.
      this->translationBuffer->addTranslation(
          pkt, cpuDelegator->getSingleThreadContext(), nullptr);
    }
  }
}

void StreamEngine::writebackElement(StreamElement *elem,
                                    StreamStoreInst *inst) {
  assert(elem->isAddrReady() && "Address should be ready.");
  auto S = elem->stream;
  assert(S->getStreamType() == ::LLVM::TDG::StreamInfo_Type_ST &&
         "Should never writeback element for non store stream.");

  // Check the bookkeeping for infly writeback memory accesses.
  assert(elem->inflyWritebackMemAccess.count(inst) == 0 &&
         "This StreamStoreInst has already been writebacked.");
  auto &inflyWritebackMemAccesses =
      elem->inflyWritebackMemAccess
          .emplace(std::piecewise_construct, std::forward_as_tuple(inst),
                   std::forward_as_tuple())
          .first->second;

  S_ELEMENT_DPRINTF(elem, "Writeback.\n");

  // hack("Send packt for stream %s.\n", S->getStreamName().c_str());

  for (size_t i = 0; i < elem->cacheBlocks; ++i) {
    auto &cacheBlockBreakdown = elem->cacheBlockBreakdownAccesses[i];

    // Translate the virtual address.
    auto vaddr = cacheBlockBreakdown.vaddr;
    auto packetSize = cacheBlockBreakdown.size;
    Addr paddr;
    if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
      panic("Failed translate vaddr %#x.\n", vaddr);
    }

    if (this->enableStreamPlacement) {
      // This means we have the placement manager.
      if (this->streamPlacementManager->access(cacheBlockBreakdown, elem,
                                               true)) {
        // Stream placement manager handles this packet.
        continue;
      }
    }

    // Allocate the book-keeping StreamMemAccess.
    auto memAccess = elem->allocateStreamMemAccess(cacheBlockBreakdown);
    inflyWritebackMemAccesses.insert(memAccess);
    // Create the writeback package.
    auto pkt = GemForgePacketHandler::createGemForgePacket(
        paddr, packetSize, memAccess, this->writebackCacheLine,
        cpuDelegator->dataRequestorId(), 0, 0);
    S->incrementInflyStreamRequest();
    this->incrementInflyStreamRequest();
    cpuDelegator->sendRequest(pkt);
  }
}

void StreamEngine::dumpFIFO() const {
  bool dumped = this->regionController->dump();
  if (dumped) {
    NO_LOC_INFORM("Total elems %d, free %d, totalRunAhead %d\n",
                  this->FIFOArray.size(), this->numFreeFIFOEntries,
                  this->getTotalRunAheadLength());
  }
}

void StreamEngine::dumpUser() const {
  for (const auto &userElement : this->userElementMap) {
    auto user = userElement.first;
    NO_LOC_INFORM("--seqNum %llu used element.\n", user);
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

void StreamEngine::receiveOffloadedLoopBoundRet(const DynStreamId &dynStreamId,
                                                int64_t tripCount,
                                                bool brokenOut) {
  this->regionController->receiveOffloadedLoopBoundRet(dynStreamId, tripCount,
                                                       brokenOut);
}

void StreamEngine::exitDump() const {
  if (streamPlacementManager != nullptr) {
    this->streamPlacementManager->dumpStreamCacheStats();
  }
  if (this->streamMap.empty()) {
    return;
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
  streamOS.flush();
  if (this->cpuDelegator->cpuId() == 0) {
    // Dump aggregated stream stats.
    auto streamStatsFileName = "stream.agg.stats.txt";
    auto &streamOS = *simout.findOrCreate(streamStatsFileName)->stream();
    for (auto &S : allStreams) {
      streamOS << S->getStreamName() << '\n';
      const auto &staticStats = StreamStatistic::getStaticStat(S->staticId);
      staticStats.dump(streamOS);
    }
    streamOS.flush();
  }
}

bool StreamEngine::isAccelerating() {
  return this->numInflyStreamConfigurations > 0;
}

bool StreamEngine::checkProgress() {
  bool hasProgress = this->numSteppedSinceLastCheck > 0 ||
                     this->numOffloadedSteppedSinceLastCheck > 0;
  // Print a warning if we are relying on offloaded streams' progress.
  if (this->numSteppedSinceLastCheck == 0 &&
      this->numOffloadedSteppedSinceLastCheck > 0) {
    SE_WARN("[Progress] Only Offloaded Progress %llu.\n",
            this->numOffloadedSteppedSinceLastCheck);
  }
  this->numSteppedSinceLastCheck = 0;
  this->numOffloadedSteppedSinceLastCheck = 0;
  return hasProgress;
}

size_t StreamEngine::getTotalRunAheadLength() const {
  size_t totalRunAheadLength = 0;
  for (const auto &IdStream : this->streamMap) {
    auto S = IdStream.second;
    if (!S->isConfigured()) {
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

bool StreamEngine::shouldCoalesceContinuousDirectMemStreamElement(
    StreamElement *elem) {
  const bool enableCoalesceContinuousElement = true;
  if (!enableCoalesceContinuousElement) {
    return false;
  }

  auto S = elem->stream;
  if (!S->isDirectMemStream()) {
    return false;
  }
  // Never do this for not floated Store/AtomicComputeStream.
  if ((S->isAtomicComputeStream() || S->isStoreComputeStream()) &&
      !elem->isElemFloatedToCache()) {
    return false;
  }
  // Check if this element is flushed.
  if (elem->flushed) {
    S_ELEMENT_DPRINTF(elem, "[NoCoalesce] Flushed.\n");
    return false;
  }

  return true;
}

void StreamEngine::coalesceContinuousDirectMemStreamElement(
    StreamElement *elem) {

  if (!this->shouldCoalesceContinuousDirectMemStreamElement(elem)) {
    return;
  }

  // Check if this is the first element.
  if (elem->FIFOIdx.entryIdx == 0) {
    return;
  }
  // Check if this is the FirstFloatElement.
  if (elem->isElemFloatedToCache() && elem->isFirstFloatElem()) {
    return;
  }
  auto S = elem->stream;
  // Get the previous element.
  auto prevElem = S->getPrevElement(elem);
  // Bail out if we have no previous element (we are the first).
  if (!prevElem) {
    S_ELEMENT_DPRINTF(elem, "[NoCoalesce] No PrevElem.\n");
    return;
  }
  // We found the previous element. Check if completely overlap.
  if ((prevElem->FIFOIdx.streamId != elem->FIFOIdx.streamId) ||
      (prevElem->FIFOIdx.entryIdx + 1 != elem->FIFOIdx.entryIdx)) {
    S_ELEMENT_PANIC(elem, "Mismatch FIFOIdx for PrevElem %s.\n",
                    prevElem->FIFOIdx);
  }

  // Check if the previous element has the cache line.
  if (!prevElem->isCacheBlockedValue) {
    S_ELEMENT_DPRINTF(elem, "[NoCoalesce] PrevElem not CacheBlocked.\n");
    return;
  }
  if (!prevElem->cacheBlocks) {
    S_ELEMENT_PANIC(elem, "No block in PrevElem.");
  }

  auto &prevElementMinBlockVAddr =
      prevElem->cacheBlockBreakdownAccesses[0].cacheBlockVAddr;
  for (int cacheBlockIdx = 0; cacheBlockIdx < elem->cacheBlocks;
       ++cacheBlockIdx) {
    auto &block = elem->cacheBlockBreakdownAccesses[cacheBlockIdx];
    assert(block.state == CacheBlockBreakdownAccess::StateE::Initialized);
    if (block.cacheBlockVAddr < prevElementMinBlockVAddr) {
      // Underflow.
      S_ELEMENT_DPRINTF(
          elem,
          "[NoCoalece] %dth Block %#x, Underflow Prev MinBlockVAddr %#x.\n",
          cacheBlockIdx, block.cacheBlockVAddr, prevElementMinBlockVAddr);
      continue;
    }
    auto blockOffset = (block.cacheBlockVAddr - prevElementMinBlockVAddr) /
                       elem->cacheBlockSize;
    if (blockOffset >= prevElem->cacheBlocks) {
      // Overflow.
      S_ELEMENT_DPRINTF(elem,
                        "[NoCoalesce] %dth Block %#x, Overflow Prev "
                        "MinBlockVAddr %#x NumBlocks %d.\n",
                        cacheBlockIdx, block.cacheBlockVAddr,
                        prevElementMinBlockVAddr, prevElem->cacheBlocks);
      continue;
    }
    /**
     * We found a match in the previous element, which means a request for
     * that line has already been sent out. There are two cases here.
     * 1. If this is a LoadStream, we try to copy the line if ready, or
     * register as a receiver if the request has not come back yet.
     * 2. If this is a StoreStream, we simply mark this block as Issued, so
     * that we won't issue duplicate requests. Note that for StoreStreams,
     * when the request comes back, it won't set the data.
     */
    const auto &prevBlock = prevElem->cacheBlockBreakdownAccesses[blockOffset];
    bool shouldCopyFromPrev = false;
    if (S->isLoadStream()) {
      shouldCopyFromPrev = true;
    }
    if (S->isAtomicComputeStream() && elem->isElemFloatedToCache()) {
      shouldCopyFromPrev = true;
    }
    if (prevBlock.state == CacheBlockBreakdownAccess::StateE::Faulted) {
      // Also mark this block faulted.
      block.state = CacheBlockBreakdownAccess::StateE::Faulted;
      elem->tryMarkValueReady();
    } else if (prevBlock.state == CacheBlockBreakdownAccess::StateE::Ready) {
      if (shouldCopyFromPrev) {
        auto offset = prevElem->mapVAddrToValueOffset(block.cacheBlockVAddr,
                                                      elem->cacheBlockSize);
        elem->setValue(block.cacheBlockVAddr, elem->cacheBlockSize,
                       &prevElem->value.at(offset));
      } else {
        // Simply mark issued.
        block.state = CacheBlockBreakdownAccess::StateE::Issued;
      }
    } else if (prevBlock.state == CacheBlockBreakdownAccess::StateE::Issued) {
      if (shouldCopyFromPrev) {
        // Register myself as a receiver.
        if (!prevBlock.memAccess) {
          S_ELEMENT_PANIC(elem,
                          "Missing memAccess for issued previous cache block.");
        }
        block.memAccess = prevBlock.memAccess;
        block.memAccess->registerReceiver(elem);
        block.state = CacheBlockBreakdownAccess::StateE::Issued;
      } else {
        // Simply mark issued.
        block.state = CacheBlockBreakdownAccess::StateE::Issued;
      }
    }
    S_ELEMENT_DPRINTF(
        elem, "[Coalesce] %dth Block %#x %s, PrevElement %dth Block %#x %s.\n",
        cacheBlockIdx, block.cacheBlockVAddr, block.state, blockOffset,
        prevBlock.cacheBlockVAddr, prevBlock.state);
  }
}

void StreamEngine::sendStreamFloatEndPacket(
    const std::vector<DynStreamId> &endedIds) {
  // We need to explicitly allocate and copy the all the ids in the packet.
  auto endedIdsCopy = new std::vector<DynStreamId>(endedIds);
  // The target address is just virtually 0 (should be set by MLC stream
  // engine).
  Addr initPAddr = 0;
  auto pkt = GemForgePacketHandler::createStreamControlPacket(
      initPAddr, cpuDelegator->dataRequestorId(), 0,
      MemCmd::Command::StreamEndReq, reinterpret_cast<uint64_t>(endedIdsCopy));
  if (Debug::CoreRubyStreamLife) {
    std::stringstream ss;
    for (const auto &id : endedIds) {
      SE_DPRINTF_(CoreRubyStreamLife, "%s: Send FloatEnd.\n", id);
    }
  }
  cpuDelegator->sendRequest(pkt);
}

void StreamEngine::sendAtomicPacket(StreamElement *element,
                                    AtomicOpFunctorPtr atomicOp) {
  if (!element->isAddrReady()) {
    S_ELEMENT_PANIC(element,
                    "Element should be address ready to send AtomicOp.");
  }
  if (element->cacheBlocks != 1) {
    S_ELEMENT_PANIC(element, "Illegal # of CacheBlocks %d for AtomicOp.",
                    element->cacheBlocks);
  }
  const auto &cacheBlockBreakdownAccess =
      element->cacheBlockBreakdownAccesses[0];
  auto vaddr = cacheBlockBreakdownAccess.vaddr;
  auto size = cacheBlockBreakdownAccess.size;
  Addr paddr;
  if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
    S_ELEMENT_PANIC(element, "Fault on AtomicOp vaddr %#x.", vaddr);
  }
  auto pkt = GemForgePacketHandler::createGemForgeAMOPacket(
      vaddr, paddr, size, nullptr /* Handler */,
      cpuDelegator->dataRequestorId(), 0 /* ContextId */, 0 /* PC */,
      std::move(atomicOp));
  auto S = element->stream;
  S->statistic.numIssuedRequest++;
  // Send the packet to translation.
  if (cpuDelegator->cpuType == GemForgeCPUDelegator::ATOMIC_SIMPLE) {
    // Directly send to memory for atomic cpu.
    this->cpuDelegator->sendRequest(pkt);
  } else {
    this->translationBuffer->addTranslation(
        pkt, cpuDelegator->getSingleThreadContext(), nullptr);
  }
}

void StreamEngine::flushPEB(Addr vaddr, int size) {
  SE_DPRINTF_(StreamAlias, "====== Flush PEB %#x, +%d.\n", vaddr, size);
  if (this->myParams->streamEngineForceNoFlushPEB) {
    warn("Forced to ignore flush PEB.");
    return;
  }
  bool foundAliasedIndirect = false;
  for (auto elem : this->peb.elements) {
    assert(elem->isAddrReady());
    assert(!elem->isFirstUserDispatched());
    if (elem->addr >= vaddr + size || elem->addr + elem->size <= vaddr) {
      // Not aliased.
      continue;
    }
    if (!elem->stream->hasNonCoreDependent()) {
      // No dependent streams.
      continue;
    }
    S_ELEMENT_DPRINTF_(StreamAlias, elem,
                       "Found AliasedIndrect PEB %#x, +%d.\n", vaddr, size);
    foundAliasedIndirect = true;
  }
  for (auto elemIter = this->peb.elements.begin(),
            elementEnd = this->peb.elements.end();
       elemIter != elementEnd;) {
    auto elem = *elemIter;
    bool aliased =
        !(elem->addr >= vaddr + size || elem->addr + elem->size <= vaddr);
    if (!aliased && !foundAliasedIndirect) {
      // Not aliased, and we are selectively flushing.
      S_ELEMENT_DPRINTF_(StreamAlias, elem, "Skip flush in PEB.\n");
      ++elemIter;
      continue;
    }
    S_ELEMENT_DPRINTF_(StreamAlias, elem, "Flushed in PEB %#x, +%d.\n",
                       elem->addr, elem->size);
    if (elem->isElemFloatedToCache()) {
      if (!elem->getStream()->isLoadStream()) {
        // This must be computation offloading.
        S_ELEMENT_PANIC(elem,
                        "Cannot flush offloaded non-load stream element.\n");
      }
    }
    if (elem->scheduledComputation) {
      /**
       * ! So far we ignore this to make sure we have prefetche distance.
       * ! The current implementation to fix this greatly limit the prefetch
       * ! distance.
       * ! See issueElements().
       */
      // S_ELEMENT_PANIC(element, "Flush in PEB when scheduled computation.");
    }

    // Clear the element to just allocate state.
    elem->flush(aliased);
    elemIter = this->peb.elements.erase(elemIter);
    // Add the dynS back to IssueList.
    this->addIssuingDynS(elem->dynS);
  }
}

void StreamEngine::RAWMisspeculate(StreamElement *element) {
  assert(!this->peb.contains(element) && "RAWMisspeculate on PEB element.");
  S_ELEMENT_DPRINTF_(StreamAlias, element, "RAWMisspeculated.\n");
  // Still, we flush the PEB when LQ misspeculate happens.
  this->flushPEB(element->addr, element->size);

  // Revert this element to just allocate state.
  element->flush(true /* aliased */);
}

void StreamEngine::resetStats() {
  for (auto &idStream : this->streamMap) {
    auto S = idStream.second;
    S->statistic.clear();
  }
}

} // namespace gem5
