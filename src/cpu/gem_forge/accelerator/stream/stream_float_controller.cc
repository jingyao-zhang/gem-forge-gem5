#include "stream_float_controller.hh"

#include "stream_region_controller.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...)                                            \
  SE_DPRINTF_(StreamFloatController, format, ##args)

#include "debug/CoreRubyStreamLife.hh"
#include "debug/StreamFloatController.hh"
#define DEBUG_TYPE StreamFloatController
#include "stream_log.hh"

StreamFloatController::StreamFloatController(
    StreamEngine *_se, std::unique_ptr<StreamFloatPolicy> _policy)
    : se(_se), policy(std::move(_policy)) {}

void StreamFloatController::floatStreams(
    const StreamConfigArgs &args, const ::LLVM::TDG::StreamRegion &region,
    std::list<DynamicStream *> &dynStreams) {

  if (!this->se->enableStreamFloat) {
    return;
  }

  if (this->se->cpuDelegator->cpuType ==
      GemForgeCPUDelegator::CPUTypeE::ATOMIC_SIMPLE) {
    SE_DPRINTF("Skip StreamFloat in AtomicSimpleCPU for %s.\n",
               region.region());
    return;
  }

  auto *cacheStreamConfigVec = new CacheStreamConfigureVec();
  StreamCacheConfigMap offloadedStreamConfigMap;
  SE_DPRINTF("Consider StreamFloat for %s.\n", region.region());

  /**
   * This is our new float decision scheme in the following order.
   * - DirectLoadStream.
   * - IndirectLoadStream.
   * - DirectStoreComputeStream/UpdateStream.
   * - ReductionStreams.
   */
  Args floatArgs(region, args.seqNum, dynStreams, offloadedStreamConfigMap,
                 *cacheStreamConfigVec);
  this->floatDirectLoadStreams(floatArgs);
  this->floatDirectAtomicComputeStreams(floatArgs);
  this->floatPointerChaseStreams(floatArgs);
  this->floatIndirectStreams(floatArgs);
  this->floatDirectStoreComputeOrUpdateStreams(floatArgs);

  // EliminatedLoop requires additional handling.
  this->floatEliminatedLoop(floatArgs);

  this->floatDirectOrPointerChaseReductionStreams(floatArgs);
  this->floatIndirectReductionStreams(floatArgs);
  this->floatTwoLevelIndirectStoreComputeStreams(floatArgs);

  // Sanity check for some offload decision.
  bool hasOffloadStoreFunc = false;
  bool hasOffloadPointerChase = false;
  bool enableFloatMem = this->se->myParams->enableFloatMem;
  for (auto &dynS : dynStreams) {
    auto S = dynS->stream;
    if (dynS->isFloatedToCache()) {
      if (S->getEnabledStoreFunc()) {
        hasOffloadStoreFunc = true;
      }
      if (S->isPointerChase()) {
        hasOffloadPointerChase = true;
      }
    } else {
      if (S->getMergedPredicatedStreams().size() > 0) {
        S_PANIC(S, "Should offload streams with merged streams.");
      }
      if (S->isMergedPredicated()) {
        S_PANIC(S, "MergedStream not offloaded.");
      }
    }
  }

  if (cacheStreamConfigVec->empty()) {
    delete cacheStreamConfigVec;
    return;
  }

  this->setFirstOffloadedElementIdx(floatArgs);

  // Send all the floating streams in one packet.
  // Dummy paddr to make ruby happy.
  Addr initPAddr = 0;
  auto pkt = GemForgePacketHandler::createStreamControlPacket(
      initPAddr, this->se->cpuDelegator->dataMasterId(), 0,
      MemCmd::Command::StreamConfigReq,
      reinterpret_cast<uint64_t>(cacheStreamConfigVec));
  if (hasOffloadStoreFunc || hasOffloadPointerChase || enableFloatMem) {
    /**
     * We have to delay this float config until StreamConfig is committed,
     * as so far we have no way to rewind the offloaded writes.
     * We also delay offloading pointer chasing streams, as they are more
     * expensive and very likely causes faults if misspeculated.
     *
     * We also delay if we have support to float to memory, as now we only
     * have partial support to handle concurrent streams in memory controller.
     */
    this->configSeqNumToDelayedFloatPktMap.emplace(args.seqNum, pkt);
    for (auto &dynS : dynStreams) {
      if (dynS->isFloatedToCache()) {
        DYN_S_DPRINTF(dynS->dynamicStreamId, "Delayed FloatConfig.\n");
        dynS->setFloatConfigDelayed(true);
      }
    }
  } else {
    for (auto &dynS : dynStreams) {
      if (dynS->isFloatedToCache()) {
        DYN_S_DPRINTF_(CoreRubyStreamLife, dynS->dynamicStreamId,
                       "Send FloatConfig.\n");
      }
    }
    this->se->cpuDelegator->sendRequest(pkt);
  }
}

void StreamFloatController::commitFloatStreams(const StreamConfigArgs &args,
                                               const StreamList &streams) {
  /**
   * We can issue the delayed float configuration now.
   * Unless this is an Midway Float, and we are waiting for the FirstFloatElem.
   */
  auto iter = this->configSeqNumToDelayedFloatPktMap.find(args.seqNum);
  if (iter == this->configSeqNumToDelayedFloatPktMap.end()) {
    return;
  }
  bool isMidwayFloat = false;
  auto pkt = iter->second;
  for (auto S : streams) {
    auto &dynS = S->getDynamicStream(args.seqNum);
    if (!dynS.isFloatedToCache()) {
      continue;
    }
    assert(dynS.isFloatConfigDelayed() && "Offload is not delayed.");
    if (dynS.getFirstFloatElemIdx() > 0) {
      isMidwayFloat = true;
      DYN_S_DPRINTF(dynS.dynamicStreamId,
                    "[MidwayFloat] CommitFloat but Wait FirstElem %llu.\n",
                    dynS.getFirstFloatElemIdx());
    } else {
      dynS.setFloatConfigDelayed(false);
      DYN_S_DPRINTF_(CoreRubyStreamLife, dynS.dynamicStreamId,
                     "Send Delayed FloatConfig.\n");
      DYN_S_DPRINTF(dynS.dynamicStreamId, "Send Delayed FloatConfig.\n");
    }
  }
  if (isMidwayFloat) {
    this->configSeqNumToMidwayFloatPktMap.emplace(args.seqNum, pkt);
  } else {
    this->se->cpuDelegator->sendRequest(pkt);
  }
  this->configSeqNumToDelayedFloatPktMap.erase(iter);
}

void StreamFloatController::rewindFloatStreams(const StreamConfigArgs &args,
                                               const StreamList &streams) {

  // Clear the delayed float packet.
  bool floatDelayed = this->configSeqNumToDelayedFloatPktMap.count(args.seqNum);
  if (floatDelayed) {
    auto pkt = this->configSeqNumToDelayedFloatPktMap.at(args.seqNum);
    auto streamConfigs = *(pkt->getPtr<CacheStreamConfigureVec *>());
    delete streamConfigs;
    delete pkt;
    this->configSeqNumToDelayedFloatPktMap.erase(args.seqNum);
  }

  std::vector<DynamicStreamId> floatedIds;
  for (auto &S : streams) {
    auto &dynS = S->getLastDynamicStream();
    if (dynS.isFloatedToCache()) {
      // Sanity check that we don't break semantics.
      DYN_S_DPRINTF(dynS.dynamicStreamId, "Rewind floated stream.\n");
      if ((S->isAtomicComputeStream() || S->isStoreComputeStream()) &&
          !dynS.isFloatConfigDelayed()) {
        DYN_S_PANIC(dynS.dynamicStreamId,
                    "Rewind a floated Atomic/StoreCompute stream.");
      }
      if (dynS.isFloatedToCacheAsRoot() && !dynS.isFloatConfigDelayed()) {
        floatedIds.push_back(dynS.dynamicStreamId);
      }
      S->statistic.numFloatRewinded++;
      dynS.setFloatedToCache(false, MachineType::MachineType_NULL);
      dynS.setFloatConfigDelayed(false);
      dynS.setFloatedToCacheAsRoot(false);
    }
  }
  if (!floatedIds.empty()) {
    this->se->sendStreamFloatEndPacket(floatedIds);
  }
}

void StreamFloatController::endFloatStreams(const DynStreamVec &dynStreams) {
  assert(!dynStreams.empty() && "No DynStream to End Float.");
  auto configSeqNum = dynStreams.front()->configSeqNum;

  auto iter = this->configSeqNumToMidwayFloatPktMap.find(configSeqNum);
  if (iter == this->configSeqNumToMidwayFloatPktMap.end()) {
    /**
     * Streams are either floated or not floated at all.
     */
    std::vector<DynamicStreamId> endedFloatRootIds;
    for (auto dynS : dynStreams) {
      /**
       * Check if this stream is offloaded and if so, send the StreamEnd
       * packet.
       */
      if (dynS->isFloatedToCacheAsRoot()) {
        assert(!dynS->isFloatConfigDelayed() &&
               "Offload still delayed when committing StreamEnd.");
        endedFloatRootIds.push_back(dynS->dynamicStreamId);
      }
    }
    // Finally send out the StreanEnd packet.
    if (!endedFloatRootIds.empty()) {
      this->se->sendStreamFloatEndPacket(endedFloatRootIds);
    }

  } else {
    /**
     * Strreams are midway floated and terminated before reaching
     * FirstFloatedElemIdx.
     */
    for (auto dynS : dynStreams) {
      if (dynS->isFloatedToCache()) {
        if (!dynS->isFloatConfigDelayed()) {
          DYN_S_PANIC(dynS->dynamicStreamId,
                      "[MidwayFloat] Not delayed anymore.");
        }
        StreamFloatPolicy::logStream(dynS->stream)
            << "[MidwayFloat] Early terminated at Element "
            << dynS->FIFOIdx.entryIdx << ".\n";
      }
    }
    auto pkt = iter->second;
    auto streamConfigs = *(pkt->getPtr<CacheStreamConfigureVec *>());
    delete streamConfigs;
    delete pkt;
    this->configSeqNumToMidwayFloatPktMap.erase(iter);
  }
}

void StreamFloatController::floatDirectLoadStreams(const Args &args) {
  auto &floatedMap = args.floatedMap;
  for (auto dynS : args.dynStreams) {
    auto S = dynS->stream;
    if (floatedMap.count(S)) {
      continue;
    }
    if (!S->isDirectLoadStream()) {
      continue;
    }
    if (S->isUpdateStream()) {
      // UpdateStream is treated more like StoreComputeStream.
      continue;
    }
    auto floatDecision = this->policy->shouldFloatStream(*dynS);
    if (!floatDecision.shouldFloat) {
      continue;
    }
    // Additional check TotalTripCount is not 0. This is only for NestStream.
    if (dynS->hasTotalTripCount() && dynS->getTotalTripCount() == 0) {
      continue;
    }

    // Get the CacheStreamConfigureData.
    auto config = S->allocateCacheConfigureData(dynS->configSeqNum);

    // Remember the offloaded decision.
    dynS->setFloatedToCacheAsRoot(true);
    dynS->setFloatedToCache(true, floatDecision.floatMachineType);
    config->offloadedMachineType = floatDecision.floatMachineType;

    this->se->numFloated++;
    floatedMap.emplace(S, config);
    args.rootConfigVec.push_back(config);

    // Remember the pseudo offloaded decision.
    if (this->se->enableStreamFloatPseudo &&
        this->policy->shouldPseudoFloatStream(*dynS)) {
      dynS->setPseudoFloatedToCache(true);
      config->isPseudoOffload = true;
    }
  }
}

void StreamFloatController::floatDirectAtomicComputeStreams(const Args &args) {
  auto &floatedMap = args.floatedMap;
  for (auto dynS : args.dynStreams) {
    auto S = dynS->stream;
    if (floatedMap.count(S)) {
      continue;
    }
    if (!S->isAtomicComputeStream() || !S->isDirectMemStream()) {
      continue;
    }
    auto floatDecision = this->policy->shouldFloatStream(*dynS);
    if (!floatDecision.shouldFloat) {
      continue;
    }

    // Get the CacheStreamConfigureData.
    auto config = S->allocateCacheConfigureData(dynS->configSeqNum);

    // Remember the offloaded decision.
    dynS->setFloatedToCacheAsRoot(true);
    dynS->setFloatedToCache(true, floatDecision.floatMachineType);
    config->offloadedMachineType = floatDecision.floatMachineType;
    this->se->numFloated++;
    floatedMap.emplace(S, config);
    args.rootConfigVec.push_back(config);

    for (auto valueBaseS : S->valueBaseStreams) {
      if (!floatedMap.count(valueBaseS)) {
        DYN_S_PANIC(dynS->dynamicStreamId, "ValueBaseS is not floated: %s.\n",
                    valueBaseS->getStreamName());
      }
      if (!valueBaseS->isDirectLoadStream()) {
        DYN_S_PANIC(dynS->dynamicStreamId,
                    "ValueBaseS is not DirectLoadStream: %s.\n",
                    valueBaseS->getStreamName());
      }
      auto &valueBaseConfig = floatedMap.at(valueBaseS);
      valueBaseConfig->addSendTo(config);
      config->addBaseOn(valueBaseConfig);
    }

    // Remember the pseudo offloaded decision.
    if (this->se->enableStreamFloatPseudo &&
        this->policy->shouldPseudoFloatStream(*dynS)) {
      dynS->setPseudoFloatedToCache(true);
      config->isPseudoOffload = true;
    }
  }
}

void StreamFloatController::floatPointerChaseStreams(const Args &args) {
  auto &floatedMap = args.floatedMap;
  for (auto dynS : args.dynStreams) {
    auto S = dynS->stream;
    if (floatedMap.count(S) || !S->isPointerChaseLoadStream()) {
      continue;
    }
    if (S->addrBaseStreams.size() != 1) {
      continue;
    }
    auto addrBaseS = *S->addrBaseStreams.begin();
    auto &addrBaseDynS = addrBaseS->getDynamicStream(dynS->configSeqNum);
    if (!addrBaseS->isPointerChaseIndVar()) {
      // AddrBaseStream is not PointerChaseIndVarStream.
      StreamFloatPolicy::logStream(S)
          << "[Not Float] due to addr base stream not pointer chase IV.\n"
          << std::flush;
      continue;
    }
    // Check if all ValueBaseStreams are floated.
    for (auto valueBaseS : S->valueBaseStreams) {
      if (!floatedMap.count(valueBaseS)) {
        // ValueBaseStream is not floated.
        StreamFloatPolicy::logStream(S)
            << "[Not Float] due to unfloat value base stream.\n"
            << std::flush;
        continue;
      }
    }
    // Check aliased store.
    if (this->checkAliasedUnpromotedStoreStream(S)) {
      continue;
    }
    // Only dependent on this direct stream.
    auto config = S->allocateCacheConfigureData(dynS->configSeqNum,
                                                true /* isIndirect */);
    auto baseConfig = addrBaseS->allocateCacheConfigureData(
        dynS->configSeqNum, true /* isIndirect */
    );
    config->isPointerChase = true;
    baseConfig->isOneIterationBehind = true;
    addrBaseDynS.setFloatedOneIterBehind(true);
    // We revert the usage edge, because in cache MemStream serves as root.
    config->addUsedBy(baseConfig);
    if (!S->valueBaseStreams.empty()) {
      S_PANIC(S, "PointerChaseStream with ValueBaseStreams is not supported.");
    }
    // Remember the decision.
    auto floatMachineType = this->policy->chooseFloatMachineType(*dynS);
    dynS->setFloatedToCache(true, floatMachineType);
    dynS->setFloatedToCacheAsRoot(true);
    config->offloadedMachineType = floatMachineType;
    this->se->numFloated++;
    addrBaseDynS.setFloatedToCache(true, floatMachineType);
    this->se->numFloated++;
    DYN_S_DPRINTF(dynS->dynamicStreamId, "Offload as pointer chase.\n");
    StreamFloatPolicy::logStream(S)
        << "[Float] " << floatMachineType << " as pointer chase.\n"
        << std::flush;
    StreamFloatPolicy::logStream(addrBaseS)
        << "[Float] " << floatMachineType << " as pointer chase IV.\n"
        << std::flush;
    floatedMap.emplace(S, config);
    floatedMap.emplace(addrBaseS, baseConfig);
    args.rootConfigVec.push_back(config);
    if (S->getEnabledStoreFunc()) {
      DYN_S_PANIC(dynS->dynamicStreamId,
                  "Computation with PointerChaseStream.");
    }
  }
}

void StreamFloatController::floatIndirectStreams(const Args &args) {
  if (!this->se->enableStreamFloatIndirect) {
    return;
  }
  auto &floatedMap = args.floatedMap;
  for (auto dynS : args.dynStreams) {
    auto S = dynS->stream;
    if (floatedMap.count(S)) {
      continue;
    }
    if (S->isDirectMemStream() || S->isPointerChase()) {
      continue;
    }
    if (!S->isLoadStream() && !S->isAtomicComputeStream()) {
      continue;
    }
    if (S->addrBaseStreams.size() != 1) {
      continue;
    }
    auto addrBaseS = *S->addrBaseStreams.begin();
    if (!floatedMap.count(addrBaseS)) {
      // AddrBaseStream is not floated.
      StreamFloatPolicy::logStream(S)
          << "[Not Float] due to unfloat addr base stream.\n"
          << std::flush;
      continue;
    }
    // Check if all ValueBaseStreams are floated.
    for (auto valueBaseS : S->valueBaseStreams) {
      if (!floatedMap.count(valueBaseS)) {
        // ValueBaseStream is not floated.
        StreamFloatPolicy::logStream(S)
            << "[Not Float] due to unfloat value base stream.\n"
            << std::flush;
        continue;
      }
    }
    /**
     * Check if there is an aliased StoreStream for this LoadStream, but
     * is not promoted into an UpdateStream.
     */
    StreamFloatPolicy::logStream(S)
        << "HasAliasedStore " << S->aliasBaseStream->hasAliasedStoreStream
        << " IsLoad " << S->isLoadStream() << " IsUpdate "
        << S->isUpdateStream() << ".\n"
        << std::flush;
    if (S->aliasBaseStream->hasAliasedStoreStream && S->isLoadStream() &&
        !S->isUpdateStream()) {
      StreamFloatPolicy::logStream(S)
          << "[Not Float] due to aliased store stream.\n"
          << std::flush;
      continue;
    }
    auto baseConfig = floatedMap.at(addrBaseS);
    // Only dependent on this direct stream.
    auto config = S->allocateCacheConfigureData(dynS->configSeqNum,
                                                true /* isIndirect */);
    baseConfig->addUsedBy(config);
    // Add SendTo edges if the ValueBaseStream is not my AddrBaseStream.
    for (auto valueBaseS : S->valueBaseStreams) {
      if (valueBaseS == addrBaseS) {
        continue;
      }
      auto &valueBaseConfig = floatedMap.at(valueBaseS);
      valueBaseConfig->addSendTo(config);
      config->addBaseOn(valueBaseConfig);
    }
    // Remember the decision.
    dynS->setFloatedToCache(true, baseConfig->offloadedMachineType);
    config->offloadedMachineType = baseConfig->offloadedMachineType;
    this->se->numFloated++;
    DYN_S_DPRINTF(dynS->dynamicStreamId, "Offload as indirect.\n");
    StreamFloatPolicy::logStream(S)
        << "[Float] " << config->offloadedMachineType << " as indirect.\n"
        << std::flush;
    floatedMap.emplace(S, config);
    if (S->getEnabledStoreFunc()) {
      if (!dynS->hasTotalTripCount()) {
        DYN_S_PANIC(dynS->dynamicStreamId,
                    "ComputeStream without TotalTripCount writes to memory.");
      }
    }
  }
}

void StreamFloatController::floatDirectStoreComputeOrUpdateStreams(
    const Args &args) {
  auto &floatedMap = args.floatedMap;
  for (auto dynS : args.dynStreams) {
    auto S = dynS->stream;
    if (floatedMap.count(S)) {
      continue;
    }
    if (!S->isDirectMemStream()) {
      continue;
    }
    if (!S->isStoreComputeStream() && !S->isUpdateStream()) {
      continue;
    }
    auto floatDecision = this->policy->shouldFloatStream(*dynS);
    if (!floatDecision.shouldFloat) {
      continue;
    }
    /**
     * Check for all the value base streams.
     */
    bool allValueBaseSFloated = true;
    for (auto valueBaseS : S->valueBaseStreams) {
      if (!floatedMap.count(valueBaseS)) {
        allValueBaseSFloated = false;
        break;
      }
    }
    if (!allValueBaseSFloated) {
      continue;
    }
    // Add SendTo edge from value base streams to myself.
    auto config = S->allocateCacheConfigureData(dynS->configSeqNum);
    for (auto valueBaseS : S->valueBaseStreams) {
      auto &valueBaseConfig = floatedMap.at(valueBaseS);
      valueBaseConfig->addSendTo(config);
      config->addBaseOn(valueBaseConfig);
    }
    // Remember the decision.
    dynS->setFloatedToCache(true, floatDecision.floatMachineType);
    dynS->setFloatedToCacheAsRoot(true);
    config->offloadedMachineType = floatDecision.floatMachineType;
    this->se->numFloated++;
    S_DPRINTF(S, "Offload DirectStoreStream.\n");
    floatedMap.emplace(S, config);
    args.rootConfigVec.push_back(config);
  }
}

void StreamFloatController::floatDirectOrPointerChaseReductionStreams(
    const Args &args) {
  auto &floatedMap = args.floatedMap;
  for (auto dynS : args.dynStreams) {
    auto S = dynS->stream;
    /**
     * DirectReductionStream: if it only uses floated affine streams.
     */
    if (floatedMap.count(S) || !S->isReduction() ||
        !S->addrBaseStreams.empty()) {
      continue;
    }
    bool allBackBaseStreamsAreAffineOrPointerChase = true;
    bool allBackBaseStreamsAreFloated = true;
    int numPointerChaseBackBaseStreams = 0;
    std::vector<CacheStreamConfigureDataPtr> backBaseStreamConfigs;
    for (auto backBaseS : S->backBaseStreams) {
      if (backBaseS == S) {
        continue;
      }
      if (!backBaseS->isDirectMemStream() &&
          !backBaseS->isPointerChaseLoadStream()) {
        allBackBaseStreamsAreAffineOrPointerChase = false;
        break;
      }
      if (backBaseS->isPointerChaseLoadStream()) {
        numPointerChaseBackBaseStreams++;
      }
      if (!floatedMap.count(backBaseS)) {
        allBackBaseStreamsAreFloated = false;
        break;
      }
      backBaseStreamConfigs.emplace_back(floatedMap.at(backBaseS));
    }
    if (!allBackBaseStreamsAreAffineOrPointerChase) {
      continue;
    }
    if (numPointerChaseBackBaseStreams > 1) {
      StreamFloatPolicy::logStream(S)
          << "[Not Float] as Multi PtrChase BackBaseStreams: "
          << numPointerChaseBackBaseStreams << ".\n"
          << std::flush;
      continue;
    }
    if (!allBackBaseStreamsAreFloated) {
      StreamFloatPolicy::logStream(S)
          << "[Not Float] as BackBaseStream is not floated.\n"
          << std::flush;
      continue;
    }
    if (backBaseStreamConfigs.empty()) {
      S_PANIC(S, "ReductionStream without BackBaseStream.");
    }
    /**
     * We require has TotalTripCount, or at Least the base
     * stream has LoopBoundCallback.
     */
    if (!dynS->hasTotalTripCount() &&
        !backBaseStreamConfigs.front()->loopBoundCallback) {
      StreamFloatPolicy::logStream(S)
          << "[Not Float] as no TotalTripCount: " << dynS->getTotalTripCount()
          << ", nor LoopBoundFunc.\n"
          << std::flush;
      return;
    }
    /**
     * Okay now we decided to float the ReductionStream. We pick the
     * affine BackBaseStream A that has most SendTo edges and make
     * all other BackBaseStreams send to that stream.
     */
    auto reductionConfig =
        S->allocateCacheConfigureData(dynS->configSeqNum, true);
    // Reduction stream is always one iteration behind.
    reductionConfig->isOneIterationBehind = true;
    dynS->setFloatedOneIterBehind(true);
    floatedMap.emplace(S, reductionConfig);

    // Search to count number of senders to each base config.
    std::vector<int> senderCount(backBaseStreamConfigs.size(), 0);
    for (const auto &config : backBaseStreamConfigs) {
      for (const auto &edge : config->depEdges) {
        if (edge.type == CacheStreamConfigureData::DepEdge::Type::SendTo) {
          const auto &receiver = edge.data;
          for (int i = 0; i < backBaseStreamConfigs.size(); ++i) {
            const auto &c = backBaseStreamConfigs[i];
            if (c->dynamicId == receiver->dynamicId) {
              senderCount[i]++;
            }
          }
        }
      }
    }

    // Select the one with the most senders.
    int maxSenders = 0;
    int baseConfigIdxWithMostSenders = 0;
    for (int i = 0; i < senderCount.size(); i++) {
      if (senderCount[i] > maxSenders) {
        maxSenders = senderCount[i];
        baseConfigIdxWithMostSenders = i;
      }
    }

    // Make all others send to that one.
    auto &baseConfigWithMostSenders =
        backBaseStreamConfigs.at(baseConfigIdxWithMostSenders);
    baseConfigWithMostSenders->addUsedBy(reductionConfig);
    S_DPRINTF(S, "ReductionStream associated with %s, existing sender %d.\n",
              baseConfigWithMostSenders->dynamicId, maxSenders);
    dynS->setFloatedToCache(true,
                            baseConfigWithMostSenders->offloadedMachineType);
    reductionConfig->offloadedMachineType =
        baseConfigWithMostSenders->offloadedMachineType;
    this->se->numFloated++;
    StreamFloatPolicy::logStream(S)
        << "[Float] " << reductionConfig->offloadedMachineType
        << " as Reduction with " << baseConfigWithMostSenders->dynamicId
        << ", existing # sender " << maxSenders << ".\n"
        << std::flush;
    for (int i = 0; i < backBaseStreamConfigs.size(); ++i) {
      if (i == baseConfigIdxWithMostSenders) {
        continue;
      }
      auto &backBaseConfig = backBaseStreamConfigs.at(i);
      backBaseConfig->addSendTo(baseConfigWithMostSenders);
      reductionConfig->addBaseOn(backBaseConfig);
    }
  }
}

void StreamFloatController::floatIndirectReductionStream(const Args &args,
                                                         DynamicStream *dynS) {
  auto &floatedMap = args.floatedMap;
  auto S = dynS->stream;
  /**
   * IndirectReductionStream:
   * 1. Only use one floated indirect stream, and maybe the floated root
   * direct stream.
   * 2. The ReductionFunc is some simple operation.
   * 3. The total trip count is long enough.
   */
  if (floatedMap.count(S) || !S->isReduction() || !S->addrBaseStreams.empty()) {
    return;
  }
  Stream *backBaseIndirectS = nullptr;
  Stream *backBaseDirectS = nullptr;
  for (auto BS : S->backBaseStreams) {
    if (BS == S) {
      // Ignore dependence on myself.
      continue;
    }
    if (BS->isIndirectLoadStream()) {
      if (backBaseIndirectS) {
        StreamFloatPolicy::logStream(S)
            << "[Not Float] as IndirectReduction with multiple "
               "BackBaseIndirectS.\n"
            << std::flush;
        return;
      }
      backBaseIndirectS = BS;
    } else if (BS->isDirectLoadStream()) {
      if (backBaseDirectS) {
        StreamFloatPolicy::logStream(S) << "[Not Float] as IndirectReduction "
                                           "with multiple BackBaseDirectS.\n"
                                        << std::flush;
        return;
      }
      backBaseDirectS = BS;
    }
  }
  if (!backBaseIndirectS) {
    S_DPRINTF(S, "Not an IndirectReductionStream to float.");
    return;
  }
  if (!this->se->myParams->enableFloatIndirectReduction) {
    StreamFloatPolicy::logStream(S)
        << "[Not Float] as IndirectReduction is disabled.\n"
        << std::flush;
    return;
  }
  if (!floatedMap.count(backBaseIndirectS)) {
    StreamFloatPolicy::logStream(S)
        << "[Not Float] as BackBaseIndirectStream is not floated.\n"
        << std::flush;
    return;
  }
  if (backBaseDirectS && !floatedMap.count(backBaseDirectS)) {
    StreamFloatPolicy::logStream(S)
        << "[Not Float] as BackBaseDirectStream is not floated.\n"
        << std::flush;
    return;
  }
  if (backBaseDirectS &&
      backBaseIndirectS->addrBaseStreams.count(backBaseDirectS) == 0) {
    StreamFloatPolicy::logStream(S)
        << "[Not Float] as BackBaseIndirectStream is not AddrDependent on "
           "BackBaseDirectStream.\n"
        << std::flush;
    return;
  }

  // Check if my reduction is simple operation.
  auto reduceOp = S->getAddrFuncComputeOp();
  if (reduceOp == ::LLVM::TDG::ExecFuncInfo_ComputeOp_FLOAT_ADD ||
      reduceOp == ::LLVM::TDG::ExecFuncInfo_ComputeOp_INT_ADD) {
    // Supported float addition.
  } else if (S->getStreamName().find("bfs_pull.cc::") != std::string::npos ||
             S->getStreamName().find("bfs_pull_shuffle.cc::") !=
                 std::string::npos) {
    /**
     * ! We manually enable this for bfs_pull.
     */
  } else {
    StreamFloatPolicy::logStream(S)
        << "[Not Float] as ReduceOp is not distributable.\n"
        << std::flush;
    return;
  }
  auto &backBaseIndirectConfig = floatedMap.at(backBaseIndirectS);
  // Check the total trip count.
  uint64_t floatIndirectReductionMinTripCount = 256;
  if (!dynS->hasTotalTripCount() ||
      dynS->getTotalTripCount() < floatIndirectReductionMinTripCount) {
    StreamFloatPolicy::logStream(S)
        << "[Not Float] as unfavorable TotalTripCount "
        << dynS->getTotalTripCount() << ".\n"
        << std::flush;
    return;
  }
  /**
   * Okay now we decided to float the IndirectReductionStream.
   */
  auto reductionConfig =
      S->allocateCacheConfigureData(dynS->configSeqNum, true);
  // Reduction stream is always one iteration behind.
  reductionConfig->isOneIterationBehind = true;
  dynS->setFloatedOneIterBehind(true);
  dynS->setFloatedToCache(true, backBaseIndirectConfig->offloadedMachineType);
  reductionConfig->offloadedMachineType =
      backBaseIndirectConfig->offloadedMachineType;
  this->se->numFloated++;
  floatedMap.emplace(S, reductionConfig);

  // Add UsedBy edge to the BackBaseIndirectS.
  backBaseIndirectConfig->addUsedBy(reductionConfig);
  S_DPRINTF(S, "Float IndirectReductionStream associated with %s.\n",
            backBaseIndirectConfig->dynamicId);
  StreamFloatPolicy::logStream(S)
      << "[Float] " << reductionConfig->offloadedMachineType
      << " as IndirectReduction with " << backBaseIndirectConfig->dynamicId
      << " TotalTripCount " << dynS->getTotalTripCount() << ".\n"
      << std::flush;

  if (backBaseDirectS) {
    /**
     * As a trick, we add a SendTo edge from BackBaseDirectS to BackBaseDirectS
     * with the IndirectReductionS as the receiver.
     */
    auto &backBaseDirectConfig = floatedMap.at(backBaseDirectS);
    backBaseDirectConfig->addSendTo(backBaseDirectConfig);
    reductionConfig->addBaseOn(backBaseDirectConfig);
    StreamFloatPolicy::logStream(S)
        << "[Float] as IndirectReduction with BackBaseDirectS "
        << backBaseDirectConfig->dynamicId << ".\n"
        << std::flush;
  }
}

void StreamFloatController::floatIndirectReductionStreams(const Args &args) {
  for (auto dynS : args.dynStreams) {
    this->floatIndirectReductionStream(args, dynS);
  }
}

void StreamFloatController::floatTwoLevelIndirectStoreComputeStreams(
    const Args &args) {
  if (!this->se->myParams->enableFloatTwoLevelIndirectStoreCompute) {
    return;
  }
  for (auto dynS : args.dynStreams) {
    this->floatTwoLevelIndirectStoreComputeStream(args, dynS);
  }
}

void StreamFloatController::floatTwoLevelIndirectStoreComputeStream(
    const Args &args, DynamicStream *dynS) {
  auto &floatedMap = args.floatedMap;
  auto S = dynS->stream;
  if (!S->isStoreComputeStream()) {
    return;
  }
  if (S->isDirectMemStream()) {
    return;
  }
  if (S->addrBaseStreams.size() != 1) {
    return;
  }
  auto addrBaseS = *S->addrBaseStreams.begin();
  if (!addrBaseS->isIndirectLoadStream()) {
    return;
  }
  if (addrBaseS->addrBaseStreams.size() != 1) {
    return;
  }
  auto addrRootS = *addrBaseS->addrBaseStreams.begin();
  if (!addrRootS->isDirectLoadStream()) {
    return;
  }
  if (!floatedMap.count(addrBaseS) || !floatedMap.count(addrRootS)) {
    StreamFloatPolicy::logStream(S)
        << "[Not Float] AddrBase/RootS not floated.\n"
        << std::flush;
    return;
  }
  for (auto valueBaseS : S->valueBaseStreams) {
    if (valueBaseS != addrBaseS && valueBaseS != addrRootS) {
      StreamFloatPolicy::logStream(S)
          << "[Not Float] ValueBaseS not one of AddrBase/RootS.\n"
          << std::flush;
      return;
    }
  }
  if (this->se->myParams->enableRangeSync) {
    StreamFloatPolicy::logStream(S)
        << "[Not Float] Two-Level IndirectStoreCompute cannot RangeSync.\n"
        << std::flush;
    return;
  }
  auto &addrBaseConfig = floatedMap.at(addrBaseS);
  auto &addrRootConfig = floatedMap.at(addrRootS);

  auto myConfig =
      S->allocateCacheConfigureData(dynS->configSeqNum, true /* isIndirect */);
  dynS->setFloatedToCache(true, addrBaseConfig->offloadedMachineType);
  myConfig->offloadedMachineType = addrBaseConfig->offloadedMachineType;
  this->se->numFloated++;
  floatedMap.emplace(S, myConfig);
  addrBaseConfig->addUsedBy(myConfig);

  StreamFloatPolicy::logStream(S)
      << "[Float] " << myConfig->offloadedMachineType
      << " as Two-Level IndirectStoreCompute associated with "
      << addrBaseConfig->dynamicId << "\n"
      << std::flush;

  // Add special SendTo edge from AddrRootS to itself if needed.
  for (auto valueBaseS : S->valueBaseStreams) {
    if (valueBaseS == addrRootS) {
      addrRootConfig->addSendTo(addrRootConfig);
      myConfig->addBaseOn(addrRootConfig);
      StreamFloatPolicy::logStream(S)
          << "[Float] as Two-Level IndirectStoreCompute based on "
          << addrRootConfig->dynamicId << "\n"
          << std::flush;
      break;
    }
  }
}

bool StreamFloatController::checkAliasedUnpromotedStoreStream(Stream *S) {
  StreamFloatPolicy::logStream(S)
      << "HasAliasedStore " << S->aliasBaseStream->hasAliasedStoreStream
      << " IsLoad " << S->isLoadStream() << " IsUpdate " << S->isUpdateStream()
      << ".\n"
      << std::flush;
  if (S->aliasBaseStream->hasAliasedStoreStream && S->isLoadStream() &&
      !S->isUpdateStream()) {
    StreamFloatPolicy::logStream(S)
        << "[Not Float] due to aliased store stream.\n"
        << std::flush;
    return true;
  }
  return false;
}

void StreamFloatController::floatEliminatedLoop(const Args &args) {

  if (!args.region.loop_eliminated()) {
    return;
  }

  auto &dynRegion =
      se->regionController->getDynRegion(args.region.region(), args.seqNum);

  /**
   * Check that LoopBound's streams all offloaded.
   */
  auto &dynBound = dynRegion.loopBound;
  auto &staticBound = dynRegion.staticRegion->loopBound;
  for (auto S : staticBound.baseStreams) {
    if (!args.floatedMap.count(S)) {
      StreamFloatPolicy::logStream(S)
          << "[Not Float] LoopBound base stream not floated.\n"
          << std::flush;
      S_DPRINTF(S, "LoopBound base stream not floated.\n");
      return;
    }
  }

  // For now, let's just support single BaseStream.
  if (staticBound.baseStreams.size() != 1) {
    SE_DPRINTF("Multiple (%lu) LoopBound base streams.\n",
               staticBound.baseStreams.size());
    return;
  }

  auto baseS = *staticBound.baseStreams.begin();
  auto &baseConfig = args.floatedMap.at(baseS);
  baseConfig->loopBoundFormalParams = dynBound.formalParams;
  baseConfig->loopBoundCallback = dynBound.boundFunc;
  baseConfig->loopBoundRet = staticBound.boundRet;

  // Remember that this bound is offloaded.
  dynBound.offloaded = true;
  StreamFloatPolicy::logStream(baseS) << "[LoopBound] Offloaded LoopBound.\n"
                                      << std::flush;
  SE_DPRINTF("[LoopBound] Offloaded LoopBound for %s.\n", args.region.region());
}

void StreamFloatController::setFirstOffloadedElementIdx(const Args &args) {
  if (!this->se->myParams->streamEngineEnableMidwayFloat) {
    return;
  }
  /**
   * Mainly work for PointerChase stream with constant first address.
   */
  uint64_t firstFloatElementIdx = 0;
  for (const auto &entry : args.floatedMap) {
    auto S = entry.first;
    if (S->isPointerChaseLoadStream()) {
      // For testing purpose, just set to 10.
      firstFloatElementIdx =
          this->se->myParams->streamEngineMidwayFloatElementIdx;
    }
  }
  if (firstFloatElementIdx == 0) {
    return;
  }
  for (auto dynS : args.dynStreams) {
    auto S = dynS->stream;
    auto iter = args.floatedMap.find(S);
    if (iter == args.floatedMap.end()) {
      continue;
    }
    StreamFloatPolicy::logStream(S)
        << "[MidwayFloat] Set to " << firstFloatElementIdx << ".\n"
        << std::flush;
    DYN_S_DPRINTF(dynS->dynamicStreamId, "FirstFloatElemIdx set to %llu.\n",
                  firstFloatElementIdx);
    dynS->setFirstFloatElemIdx(firstFloatElementIdx);
    args.floatedMap.at(dynS->stream)->firstFloatElementIdx =
        firstFloatElementIdx;
  }

  // Check LoopBound.
  auto &dynRegion = this->se->regionController->getDynRegion(
      "SetFirstOffloadedElementIdx", args.seqNum);
  auto &dynBound = dynRegion.loopBound;
  if (dynBound.offloaded) {
    SE_DPRINTF("[MidwayFloat][LoopBound] FirstFloatElemIdx set to %llu.\n",
               firstFloatElementIdx);
    dynBound.offloadedFirstElementIdx = firstFloatElementIdx;
  }
}

void StreamFloatController::processMidwayFloat() {
  for (auto iter = this->configSeqNumToMidwayFloatPktMap.begin(),
            end = this->configSeqNumToMidwayFloatPktMap.end();
       iter != end;) {
    if (this->trySendMidwayFloat(iter)) {
      iter = this->configSeqNumToMidwayFloatPktMap.erase(iter);
    } else {
      ++iter;
    }
  }
}

bool StreamFloatController::isMidwayFloatReady(
    CacheStreamConfigureDataPtr &config) {
  auto S = config->stream;
  auto dynS = S->getDynamicStream(config->dynamicId);
  if (!dynS) {
    DYN_S_PANIC(config->dynamicId,
                "[MidwayFloat] DynS released before MidwayFloat released.");
  }
  if (dynS->FIFOIdx.entryIdx <= config->firstFloatElementIdx) {
    DYN_S_DPRINTF(dynS->dynamicStreamId,
                  "[MidwayFloat] DynS TailElem %llu < FirstFloatElem %llu. "
                  "Not Yet Float.\n",
                  dynS->FIFOIdx.entryIdx, config->firstFloatElementIdx);
    return false;
  }
  /**
   * If there is StreamLoopBound, also check that LoopBound has evaluated.
   */
  auto &dynRegion = this->se->regionController->getDynRegion(
      "[MidwayFloat] Check LoopBound before MidwayFloat", dynS->configSeqNum);
  if (dynRegion.staticRegion->region.is_loop_bound()) {
    auto &dynBound = dynRegion.loopBound;
    if (dynBound.nextElementIdx > config->firstFloatElementIdx) {
      DYN_S_PANIC(dynS->dynamicStreamId,
                  "[MidwayFloat] Impossible! LoopBound NextElem %llu > "
                  "FirstFloatElem %llu.",
                  dynBound.nextElementIdx, config->firstFloatElementIdx);
    }
    if (dynBound.nextElementIdx < config->firstFloatElementIdx) {
      DYN_S_DPRINTF(dynS->dynamicStreamId,
                    "[MidwayFloat] LoopBound NextElem %llu < FirstFloatElem "
                    "%llu. Not Yet Float.\n",
                    dynBound.nextElementIdx, config->firstFloatElementIdx);
      return false;
    }
    if (dynBound.brokenOut) {
      DYN_S_DPRINTF(dynS->dynamicStreamId,
                    "[MidwayFloat] LoopBound BrokenOut NextElem %llu <= "
                    "FirstFloatElem %llu. Don't Float.\n",
                    dynBound.nextElementIdx, config->firstFloatElementIdx);
      return false;
    }
  }
  if (S->isReduction() || S->isPointerChaseIndVar()) {
    /**
     * Since these streams are one iteration behind, we require them to be value
     * ready.
     */
    auto firstFloatElement =
        dynS->getElementByIdx(config->firstFloatElementIdx);
    if (!firstFloatElement) {
      DYN_S_PANIC(dynS->dynamicStreamId,
                  "[MidwayFloat] FirstFloatElem already released.");
    }
    if (!firstFloatElement->isValueReady) {
      S_ELEMENT_DPRINTF(
          firstFloatElement,
          "[MidwayFloat] FirstFloatElem of Reduce/PtrChase not ValueReady.\n");
      return false;
    }
  }
  for (auto &depEdge : config->depEdges) {
    auto &depConfig = depEdge.data;
    if (!this->isMidwayFloatReady(depConfig)) {
      return false;
    }
  }
  return true;
}

bool StreamFloatController::trySendMidwayFloat(SeqNumToPktMapIter iter) {
  auto configSeqNum = iter->first;
  auto pkt = iter->second;
  auto configVec = *(pkt->getPtr<CacheStreamConfigureVec *>());
  bool readyToFloat = true;
  for (auto &config : *configVec) {
    if (!this->isMidwayFloatReady(config)) {
      readyToFloat = false;
      break;
    }
  }

  if (readyToFloat) {
    auto &dynRegion = this->se->regionController->getDynRegion(
        to_string(configVec->front()->dynamicId), configSeqNum);
    for (auto S : dynRegion.staticRegion->streams) {
      auto &dynS = S->getDynamicStream(configSeqNum);
      dynS.setFloatConfigDelayed(false);
      DYN_S_DPRINTF_(CoreRubyStreamLife, dynS.dynamicStreamId,
                     "[MidwayFloat] Midway Floated at Elem %llu.\n",
                     dynS.getFirstFloatElemIdx());
      DYN_S_DPRINTF(dynS.dynamicStreamId,
                    "[MidwayFloat] Midway Floated at Elem %llu.\n",
                    dynS.getFirstFloatElemIdx());
    }
    this->se->cpuDelegator->sendRequest(pkt);
  }

  return readyToFloat;
}