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
    std::list<DynStream *> &dynStreams) {

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

  if (cacheStreamConfigVec->empty()) {
    delete cacheStreamConfigVec;
    return;
  }

  this->policy->setFloatPlans(floatArgs.dynStreams, floatArgs.floatedMap,
                              floatArgs.rootConfigVec);

  this->setLoopBoundFirstOffloadedElementIdx(floatArgs);
  this->propagateFloatPlan(floatArgs);

  /**
   * Sanity check for some offload decision.
   * Since here we check isFloatedToCache(), which is set by setFloatPlans(),
   * we have to perform this check after that.
   */
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

  bool hasOffloadFaultedInitPAddr = false;
  for (const auto &config : *cacheStreamConfigVec) {
    if (!config->initPAddrValid) {
      hasOffloadFaultedInitPAddr = true;
    }
  }

  // Send all the floating streams in one packet.
  // Dummy paddr to make ruby happy.
  Addr initPAddr = 0;
  auto pkt = GemForgePacketHandler::createStreamControlPacket(
      initPAddr, this->se->cpuDelegator->dataMasterId(), 0,
      MemCmd::Command::StreamConfigReq,
      reinterpret_cast<uint64_t>(cacheStreamConfigVec));
  if (hasOffloadStoreFunc || hasOffloadPointerChase ||
      hasOffloadFaultedInitPAddr || enableFloatMem) {
    /**
     * There are some scenarios we want to delay offloading until StreamConfig
     * is committed.
     *
     * 1. There are writes offloaded, as far we have no way to rewind them.
     * 2. We are offloading pointer chasing streams, as they are more
     * expensive and very likely causes faults if misspeculated.
     * 3. Some stream has the initial address faulted, which is very likely
     * caused by misspeculated StreamConfig.
     *
     * We also delay if we have support to float to memory, as now we only
     * have partial support to handle concurrent streams in memory controller.
     */
    this->configSeqNumToDelayedFloatPktMap.emplace(args.seqNum, pkt);
    for (auto &dynS : dynStreams) {
      if (dynS->isFloatedToCache()) {
        DYN_S_DPRINTF(dynS->dynStreamId, "Delayed FloatConfig.\n");
        dynS->setFloatConfigDelayed(true);
      }
    }
  } else {
    for (auto &dynS : dynStreams) {
      if (dynS->isFloatedToCache()) {
        DYN_S_DPRINTF_(CoreRubyStreamLife, dynS->dynStreamId,
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
    auto &dynS = S->getDynStream(args.seqNum);
    if (!dynS.isFloatedToCache()) {
      continue;
    }
    assert(dynS.isFloatConfigDelayed() && "Offload is not delayed.");
    if (dynS.getFirstFloatElemIdx() > 0) {
      isMidwayFloat = true;
      DYN_S_DPRINTF(dynS.dynStreamId,
                    "[MidwayFloat] CommitFloat but Wait FirstElem %llu.\n",
                    dynS.getFirstFloatElemIdx());
    } else {
      dynS.setFloatConfigDelayed(false);
      DYN_S_DPRINTF_(CoreRubyStreamLife, dynS.dynStreamId,
                     "Send Delayed FloatConfig.\n");
      DYN_S_DPRINTF(dynS.dynStreamId, "Send Delayed FloatConfig.\n");
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

  std::vector<DynStreamId> floatedIds;
  for (auto &S : streams) {
    auto &dynS = S->getDynStream(args.seqNum);
    if (dynS.isFloatedToCache()) {
      // Sanity check that we don't break semantics.
      DYN_S_DPRINTF(dynS.dynStreamId, "Rewind floated stream.\n");
      if ((S->isAtomicComputeStream() || S->isStoreComputeStream()) &&
          !dynS.isFloatConfigDelayed()) {
        DYN_S_PANIC(dynS.dynStreamId,
                    "Rewind a floated Atomic/StoreCompute stream.");
      }
      if (dynS.isFloatedToCacheAsRoot() && !dynS.isFloatConfigDelayed()) {
        floatedIds.push_back(dynS.dynStreamId);
      }
      S->statistic.numFloatRewinded++;
      dynS.setFloatedToCache(false);
      dynS.setFloatConfigDelayed(false);
      dynS.setFloatedToCacheAsRoot(false);
      dynS.getFloatPlan().clear();
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
    std::vector<DynStreamId> endedFloatRootIds;
    for (auto dynS : dynStreams) {
      /**
       * Check if this stream is offloaded and if so, send the StreamEnd
       * packet.
       */
      if (dynS->isFloatedToCacheAsRoot()) {
        assert(!dynS->isFloatConfigDelayed() &&
               "Offload still delayed when committing StreamEnd.");
        endedFloatRootIds.push_back(dynS->dynStreamId);
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
          DYN_S_PANIC(dynS->dynStreamId, "[MidwayFloat] Not delayed anymore.");
        }
        StreamFloatPolicy::logS(*dynS)
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
    if (S->isDelayIssueUntilFIFOHead()) {
      // Delayed streams usually has alias problem. Do not offload.
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
    if (dynS->hasZeroTripCount()) {
      continue;
    }

    // Get the CacheStreamConfigureData.
    auto config = S->allocateCacheConfigureData(dynS->configSeqNum);

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

    floatedMap.emplace(S, config);
    args.rootConfigVec.push_back(config);

    for (auto valueBaseS : S->valueBaseStreams) {
      if (!floatedMap.count(valueBaseS)) {
        DYN_S_PANIC(dynS->dynStreamId, "ValueBaseS is not floated: %s.\n",
                    valueBaseS->getStreamName());
      }
      if (!valueBaseS->isDirectLoadStream()) {
        DYN_S_PANIC(dynS->dynStreamId,
                    "ValueBaseS is not DirectLoadStream: %s.\n",
                    valueBaseS->getStreamName());
      }
      auto &valueBaseConfig = floatedMap.at(valueBaseS);
      auto reuse = dynS->getBaseElemReuseCount(valueBaseS);
      valueBaseConfig->addSendTo(config, reuse);
      config->addBaseOn(valueBaseConfig, reuse);
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
    auto &addrBaseDynS = addrBaseS->getDynStream(dynS->configSeqNum);
    if (!addrBaseS->isPointerChaseIndVar()) {
      // AddrBaseStream is not PointerChaseIndVarStream.
      StreamFloatPolicy::logS(*dynS)
          << "[Not Float] due to addr base stream not pointer chase IV.\n"
          << std::flush;
      continue;
    }
    // Check if all ValueBaseStreams are floated.
    for (auto valueBaseS : S->valueBaseStreams) {
      if (!floatedMap.count(valueBaseS)) {
        // ValueBaseStream is not floated.
        StreamFloatPolicy::logS(*dynS)
            << "[Not Float] due to unfloat value base stream.\n"
            << std::flush;
        continue;
      }
    }
    // Check aliased store.
    if (this->checkAliasedUnpromotedStoreStream(dynS)) {
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
    config->addUsedBy(baseConfig, 1 /* reuse */);
    if (!S->valueBaseStreams.empty()) {
      S_PANIC(S, "PointerChaseStream with ValueBaseStreams is not supported.");
    }
    // Remember the decision.
    DYN_S_DPRINTF(dynS->dynStreamId, "Offload as pointer chase.\n");
    StreamFloatPolicy::logS(*dynS) << "[Float] as pointer chase.\n"
                                   << std::flush;
    StreamFloatPolicy::logS(addrBaseDynS) << "[Float] as pointer chase IV.\n"
                                          << std::flush;
    floatedMap.emplace(S, config);
    floatedMap.emplace(addrBaseS, baseConfig);
    args.rootConfigVec.push_back(config);
    if (S->getEnabledStoreFunc()) {
      DYN_S_PANIC(dynS->dynStreamId, "Computation with PointerChaseStream.");
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
      StreamFloatPolicy::logS(*dynS)
          << "[Not Float] due to unfloat addr base stream.\n"
          << std::flush;
      continue;
    }
    // Check if all ValueBaseStreams are floated.
    for (auto valueBaseS : S->valueBaseStreams) {
      if (!floatedMap.count(valueBaseS)) {
        // ValueBaseStream is not floated.
        StreamFloatPolicy::logS(*dynS)
            << "[Not Float] due to unfloat value base stream.\n"
            << std::flush;
        continue;
      }
    }
    /**
     * Check if there is an aliased StoreStream for this LoadStream, but
     * is not promoted into an UpdateStream.
     */
    StreamFloatPolicy::logS(*dynS)
        << "HasAliasedStore " << S->aliasBaseStream->hasAliasedStoreStream
        << " IsLoad " << S->isLoadStream() << " IsUpdate "
        << S->isUpdateStream() << ".\n"
        << std::flush;
    if (S->aliasBaseStream->hasAliasedStoreStream && S->isLoadStream() &&
        !S->isUpdateStream()) {
      StreamFloatPolicy::logS(*dynS)
          << "[Not Float] due to aliased store stream.\n"
          << std::flush;
      continue;
    }
    auto baseConfig = floatedMap.at(addrBaseS);
    // Only dependent on this direct stream.
    auto config = S->allocateCacheConfigureData(dynS->configSeqNum,
                                                true /* isIndirect */);
    baseConfig->addUsedBy(config, 1 /* reuse */);
    // Add SendTo edges if the ValueBaseStream is not my AddrBaseStream.
    for (auto valueBaseS : S->valueBaseStreams) {
      if (valueBaseS == addrBaseS) {
        continue;
      }
      auto &valueBaseConfig = floatedMap.at(valueBaseS);
      auto reuse = dynS->getBaseElemReuseCount(valueBaseS);
      valueBaseConfig->addSendTo(config, reuse);
      config->addBaseOn(valueBaseConfig, reuse);
    }
    DYN_S_DPRINTF(dynS->dynStreamId, "Offload as indirect.\n");
    StreamFloatPolicy::logS(*dynS) << "[Float] as indirect.\n" << std::flush;
    floatedMap.emplace(S, config);
    if (S->getEnabledStoreFunc()) {
      if (!dynS->hasTotalTripCount()) {
        DYN_S_PANIC(dynS->dynStreamId,
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
      auto reuse = dynS->getBaseElemReuseCount(valueBaseS);
      valueBaseConfig->addSendTo(config, reuse);
      config->addBaseOn(valueBaseConfig, reuse);
    }
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
      StreamFloatPolicy::logS(*dynS)
          << "[Not Float] as Multi PtrChase BackBaseStreams: "
          << numPointerChaseBackBaseStreams << ".\n"
          << std::flush;
      continue;
    }
    if (!allBackBaseStreamsAreFloated) {
      StreamFloatPolicy::logS(*dynS)
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
      StreamFloatPolicy::logS(*dynS)
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
    baseConfigWithMostSenders->addUsedBy(reductionConfig, 1 /* reuse */);
    S_DPRINTF(S, "ReductionStream associated with %s, existing sender %d.\n",
              baseConfigWithMostSenders->dynamicId, maxSenders);
    StreamFloatPolicy::logS(*dynS)
        << "[Float] as Reduction with " << baseConfigWithMostSenders->dynamicId
        << ", existing # sender " << maxSenders << ".\n"
        << std::flush;
    for (int i = 0; i < backBaseStreamConfigs.size(); ++i) {
      if (i == baseConfigIdxWithMostSenders) {
        continue;
      }
      auto &backBaseConfig = backBaseStreamConfigs.at(i);
      auto reuse = dynS->getBaseElemReuseCount(backBaseConfig->stream);
      backBaseConfig->addSendTo(baseConfigWithMostSenders, reuse);
      reductionConfig->addBaseOn(backBaseConfig, reuse);
    }
  }
}

void StreamFloatController::floatIndirectReductionStream(const Args &args,
                                                         DynStream *dynS) {
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
        StreamFloatPolicy::logS(*dynS)
            << "[Not Float] as IndirectReduction with multiple "
               "BackBaseIndirectS.\n"
            << std::flush;
        return;
      }
      backBaseIndirectS = BS;
    } else if (BS->isDirectLoadStream()) {
      if (backBaseDirectS) {
        StreamFloatPolicy::logS(*dynS) << "[Not Float] as IndirectReduction "
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
    StreamFloatPolicy::logS(*dynS)
        << "[Not Float] as IndirectReduction is disabled.\n"
        << std::flush;
    return;
  }
  if (!floatedMap.count(backBaseIndirectS)) {
    StreamFloatPolicy::logS(*dynS)
        << "[Not Float] as BackBaseIndirectStream is not floated.\n"
        << std::flush;
    return;
  }
  if (backBaseDirectS && !floatedMap.count(backBaseDirectS)) {
    StreamFloatPolicy::logS(*dynS)
        << "[Not Float] as BackBaseDirectStream is not floated.\n"
        << std::flush;
    return;
  }
  if (backBaseDirectS &&
      backBaseIndirectS->addrBaseStreams.count(backBaseDirectS) == 0) {
    StreamFloatPolicy::logS(*dynS)
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
  } else if (S->getStreamName().find("bfs_pull") != std::string::npos) {
    /**
     * ! We manually enable this for bfs_pull.
     */
  } else {
    StreamFloatPolicy::logS(*dynS)
        << "[Not Float] as ReduceOp is not distributable.\n"
        << std::flush;
    return;
  }
  auto &backBaseIndirectConfig = floatedMap.at(backBaseIndirectS);
  // Check the total trip count.
  uint64_t floatIndirectReductionMinTripCount = 256;
  if (!dynS->hasTotalTripCount() ||
      dynS->getTotalTripCount() < floatIndirectReductionMinTripCount) {
    StreamFloatPolicy::logS(*dynS)
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
  floatedMap.emplace(S, reductionConfig);

  // Add UsedBy edge to the BackBaseIndirectS.
  backBaseIndirectConfig->addUsedBy(reductionConfig, 1 /* reuse */);
  S_DPRINTF(S, "Float IndirectReductionStream associated with %s.\n",
            backBaseIndirectConfig->dynamicId);
  StreamFloatPolicy::logS(*dynS)
      << "[Float] as IndirectReduction with "
      << backBaseIndirectConfig->dynamicId << " TotalTripCount "
      << dynS->getTotalTripCount() << ".\n"
      << std::flush;

  if (backBaseDirectS) {
    /**
     * As a trick, we add a SendTo edge from BackBaseDirectS to BackBaseDirectS
     * with the IndirectReductionS as the receiver.
     */
    auto &backBaseDirectConfig = floatedMap.at(backBaseDirectS);
    auto reuse = dynS->getBaseElemReuseCount(backBaseDirectConfig->stream);
    backBaseDirectConfig->addSendTo(backBaseDirectConfig, reuse);
    reductionConfig->addBaseOn(backBaseDirectConfig, reuse);
    StreamFloatPolicy::logS(*dynS)
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
    const Args &args, DynStream *dynS) {
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
    StreamFloatPolicy::logS(*dynS)
        << "[Not Float] AddrBase/RootS not floated.\n"
        << std::flush;
    return;
  }
  for (auto valueBaseS : S->valueBaseStreams) {
    if (valueBaseS != addrBaseS && valueBaseS != addrRootS) {
      StreamFloatPolicy::logS(*dynS)
          << "[Not Float] ValueBaseS not one of AddrBase/RootS.\n"
          << std::flush;
      return;
    }
  }
  if (this->se->myParams->enableRangeSync) {
    StreamFloatPolicy::logS(*dynS)
        << "[Not Float] Two-Level IndirectStoreCompute cannot RangeSync.\n"
        << std::flush;
    return;
  }
  auto &addrBaseConfig = floatedMap.at(addrBaseS);
  auto &addrRootConfig = floatedMap.at(addrRootS);

  auto myConfig =
      S->allocateCacheConfigureData(dynS->configSeqNum, true /* isIndirect */);
  floatedMap.emplace(S, myConfig);
  addrBaseConfig->addUsedBy(myConfig, 1 /* Reuse */);

  StreamFloatPolicy::logS(*dynS)
      << "[Float] as Two-Level IndirectStoreCompute associated with "
      << addrBaseConfig->dynamicId << "\n"
      << std::flush;

  // Add special SendTo edge from AddrRootS to itself if needed.
  for (auto valueBaseS : S->valueBaseStreams) {
    if (valueBaseS == addrRootS) {
      auto reuse = dynS->getBaseElemReuseCount(valueBaseS);
      addrRootConfig->addSendTo(addrRootConfig, reuse);
      myConfig->addBaseOn(addrRootConfig, reuse);
      StreamFloatPolicy::logS(*dynS)
          << "[Float] as Two-Level IndirectStoreCompute based on "
          << addrRootConfig->dynamicId << "\n"
          << std::flush;
      break;
    }
  }
}

bool StreamFloatController::checkAliasedUnpromotedStoreStream(DynStream *dynS) {
  auto S = dynS->stream;
  StreamFloatPolicy::logS(*dynS)
      << "HasAliasedStore " << S->aliasBaseStream->hasAliasedStoreStream
      << " IsLoad " << S->isLoadStream() << " IsUpdate " << S->isUpdateStream()
      << ".\n"
      << std::flush;
  if (S->aliasBaseStream->hasAliasedStoreStream && S->isLoadStream() &&
      !S->isUpdateStream()) {
    StreamFloatPolicy::logS(*dynS)
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
      StreamFloatPolicy::logS(S->getDynStream(args.seqNum))
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
  auto &baseDynS = baseS->getDynStream(args.seqNum);
  auto &baseConfig = args.floatedMap.at(baseS);
  baseConfig->loopBoundFormalParams = dynBound.formalParams;
  baseConfig->loopBoundCallback = dynBound.boundFunc;
  baseConfig->loopBoundRet = staticBound.boundRet;

  // Remember that this bound is offloaded.
  dynBound.offloaded = true;
  StreamFloatPolicy::logS(baseDynS) << "[LoopBound] Offloaded LoopBound.\n"
                                    << std::flush;
  SE_DPRINTF("[LoopBound] Offloaded LoopBound for %s.\n", args.region.region());
}

void StreamFloatController::setLoopBoundFirstOffloadedElementIdx(
    const Args &args) {
  /**
   * Just take FirstFloatElemIdx from the FloatPlan of stream.
   */
  uint64_t firstFloatElementIdx = 0;
  for (auto dynS : args.dynStreams) {
    auto S = dynS->stream;
    if (args.floatedMap.count(S)) {
      firstFloatElementIdx = dynS->getFirstFloatElemIdx();
      StreamFloatPolicy::logS(*dynS) << "[MidwayFloat] Get FirstFloatElemIdx "
                                     << firstFloatElementIdx << ".\n"
                                     << std::flush;
      DYN_S_DPRINTF(dynS->dynStreamId, "Get FirstFloatElemIdx %llu.\n",
                    firstFloatElementIdx);
      break;
    }
  }
  if (firstFloatElementIdx == 0) {
    return;
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

void StreamFloatController::propagateFloatPlan(const Args &args) {
  for (auto dynS : args.dynStreams) {
    auto S = dynS->stream;
    auto iter = args.floatedMap.find(S);
    if (iter == args.floatedMap.end()) {
      continue;
    }
    dynS->getFloatPlan().finalize();
    StreamFloatPolicy::logS(*dynS)
        << "[FloatPlan] Finalized as " << dynS->getFloatPlan() << ".\n"
        << std::flush;
    DYN_S_DPRINTF(dynS->dynStreamId, "Propagate FloatPlan %s.\n",
                  dynS->getFloatPlan());
    iter->second->floatPlan = dynS->getFloatPlan();
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
  auto dynS = S->getDynStream(config->dynamicId);
  if (!dynS) {
    DYN_S_PANIC(config->dynamicId,
                "[MidwayFloat] DynS released before MidwayFloat released.");
  }
  auto firstFloatElementIdx = config->floatPlan.getFirstFloatElementIdx();
  if (dynS->FIFOIdx.entryIdx <= firstFloatElementIdx) {
    DYN_S_DPRINTF(dynS->dynStreamId,
                  "[MidwayFloat] DynS TailElem %llu < FirstFloatElem %llu. "
                  "Not Yet Float.\n",
                  dynS->FIFOIdx.entryIdx, firstFloatElementIdx);
    return false;
  }
  /**
   * If there is StreamLoopBound, also check that LoopBound has evaluated.
   */
  auto &dynRegion = this->se->regionController->getDynRegion(
      "[MidwayFloat] Check LoopBound before MidwayFloat", dynS->configSeqNum);
  if (dynRegion.staticRegion->region.is_loop_bound()) {
    auto &dynBound = dynRegion.loopBound;
    if (dynBound.nextElemIdx > firstFloatElementIdx) {
      DYN_S_PANIC(dynS->dynStreamId,
                  "[MidwayFloat] Impossible! LoopBound NextElem %llu > "
                  "FirstFloatElem %llu.",
                  dynBound.nextElemIdx, firstFloatElementIdx);
    }
    if (dynBound.nextElemIdx < firstFloatElementIdx) {
      DYN_S_DPRINTF(dynS->dynStreamId,
                    "[MidwayFloat] LoopBound NextElem %llu < FirstFloatElem "
                    "%llu. Not Yet Float.\n",
                    dynBound.nextElemIdx, firstFloatElementIdx);
      return false;
    }
    if (dynBound.brokenOut) {
      DYN_S_DPRINTF(dynS->dynStreamId,
                    "[MidwayFloat] LoopBound BrokenOut NextElem %llu <= "
                    "FirstFloatElem %llu. Don't Float.\n",
                    dynBound.nextElemIdx, firstFloatElementIdx);
      return false;
    }
  }
  if (S->isReduction() || S->isPointerChaseIndVar()) {
    /**
     * Since these streams are one iteration behind, we require them to be value
     * ready.
     */
    auto firstFloatElement = dynS->getElementByIdx(firstFloatElementIdx);
    if (!firstFloatElement) {
      DYN_S_PANIC(dynS->dynStreamId,
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
      auto &dynS = S->getDynStream(configSeqNum);
      dynS.setFloatConfigDelayed(false);
      DYN_S_DPRINTF_(CoreRubyStreamLife, dynS.dynStreamId,
                     "[MidwayFloat] Midway Floated at Elem %llu.\n",
                     dynS.getFirstFloatElemIdx());
      DYN_S_DPRINTF(dynS.dynStreamId,
                    "[MidwayFloat] Midway Floated at Elem %llu.\n",
                    dynS.getFirstFloatElemIdx());
    }
    this->se->cpuDelegator->sendRequest(pkt);
  }

  return readyToFloat;
}