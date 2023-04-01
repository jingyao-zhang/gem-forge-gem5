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

namespace gem5 {

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
  SE_DPRINTF(">>> Consider Float DirectLoadStreams.\n");
  this->floatDirectLoadStreams(floatArgs);
  SE_DPRINTF(">>> Consider Float DirectAtomicComputeStreams.\n");
  this->floatDirectAtomicComputeStreams(floatArgs);
  SE_DPRINTF(">>> Consider Float PointerChaseStreams.\n");
  this->floatPointerChaseStreams(floatArgs);
  SE_DPRINTF(">>> Consider Float IndirectStreams.\n");
  this->floatIndStreams(floatArgs);
  SE_DPRINTF(">>> Consider Float DirectUpdateStreams.\n");
  this->floatDirectUpdateStreams(floatArgs);

  // EliminatedLoop requires additional handling.
  SE_DPRINTF(">>> Handle Float EliminatedLoop.\n");
  this->floatEliminatedLoop(floatArgs);

  SE_DPRINTF(">>> Consider Float DirectOrPointerChaseReductionStreams.\n");
  this->floatDirectOrPtrChaseReduceStreams(floatArgs);
  SE_DPRINTF(">>> Consider Float DirectStoreComputeStreams.\n");
  this->floatDirectStoreComputeStreams(floatArgs);
  /**
   * Reconsider FloatDirectUpdateStreams as some may depend on
   * InnerLoopReductionStream. For example: gfm.kmeans_pum.
   */
  SE_DPRINTF(">>> Consider Float DirectUpdateStreams ... 2.\n");
  this->floatDirectUpdateStreams(floatArgs);
  SE_DPRINTF(">>> Consider Float IndirectReductionStreams.\n");
  this->floatIndirectReductionStreams(floatArgs);
  SE_DPRINTF(">>> Consider Float MultiLevelIndirectStoreComputeStreams.\n");
  this->floatMultiLevelIndirectStoreComputeStreams(floatArgs);
  SE_DPRINTF(">>> Decide MLCBufferNumSlices.\n");
  this->decideMLCBufferNumSlices(floatArgs);

  if (cacheStreamConfigVec->empty()) {
    delete cacheStreamConfigVec;
    return;
  }

  this->policy->setFloatPlans(floatArgs.dynStreams, floatArgs.floatedMap,
                              floatArgs.rootConfigVec);

  this->setLoopBoundFirstOffloadedElemIdx(floatArgs);
  this->propagateFloatPlan(floatArgs);

  /**
   * Sanity check for some offload decision.
   * Since here we check isFloatedToCache(), which is set by setFloatPlans(),
   * we have to perform this check after that.
   */
  bool hasOffloadStoreFunc = false;
  bool hasOffloadPointerChase = false;
  bool enableFloatMem = this->se->myParams->enableFloatMem;
  bool hasDepNestRegion = false;
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
    if (!this->se->regionController
             ->getDynRegion("FloatStream", dynS->configSeqNum)
             .nestConfigs.empty()) {
      hasDepNestRegion = true;
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
      initPAddr, this->se->cpuDelegator->dataRequestorId(), 0,
      MemCmd::Command::StreamConfigReq,
      reinterpret_cast<uint64_t>(cacheStreamConfigVec));
  if (hasOffloadStoreFunc || hasOffloadPointerChase ||
      hasOffloadFaultedInitPAddr || enableFloatMem || hasDepNestRegion) {
    /**
     * There are some scenarios we want to delay offloading until StreamConfig
     * is committed.
     *
     * 1. There are writes offloaded, as far we have no way to rewind them.
     * 2. We are offloading pointer chasing streams, as they are more
     * expensive and very likely causes faults if misspeculated.
     * 3. Some stream has the initial address faulted, which is very likely
     * caused by misspeculated StreamConfig.
     * 4. We have dependent NestRegion.
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
      // Update all StreamElement info.
      dynS.updateFloatInfoForElems();
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
  std::vector<DynStream *> floatedDirectLoadStreams;
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

    floatedDirectLoadStreams.push_back(dynS);
    floatedMap.emplace(S, config);
    args.rootConfigVec.push_back(config);

    // Remember the pseudo offloaded decision.
    if (this->se->enableStreamFloatPseudo &&
        this->policy->shouldPseudoFloatStream(*dynS)) {
      dynS->setPseudoFloatedToCache(true);
      config->isPseudoOffload = true;
    }
  }

  // Handle forwarding dependence for LoadComputeStream.
  for (auto dynS : floatedDirectLoadStreams) {
    auto S = dynS->stream;
    auto config = floatedMap.at(S);
    for (const auto &baseEdge : dynS->baseEdges) {
      if (baseEdge.type != DynStream::StreamDepEdge::TypeE::Value) {
        continue;
      }
      auto baseS = this->se->getStream(baseEdge.baseStaticId);
      auto baseConfigIter = floatedMap.find(baseS);
      assert(baseConfigIter != floatedMap.end() && "BaseValueS not floated.");

      assert(baseS != S);
      auto baseConfig = baseConfigIter->second;
      baseConfig->addSendTo(config, baseEdge.baseElemReuseCnt,
                            baseEdge.baseElemSkipCnt);
      config->addBaseOn(baseConfig, baseEdge.baseElemReuseCnt,
                        baseEdge.baseElemSkipCnt);
      StreamFloatPolicy::logS(baseConfig->dynamicId)
          << "[Send] to DirectLoadComputeS " << config->dynamicId << '\n'
          << std::flush;
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
      auto skip = dynS->getBaseElemSkipCount(valueBaseS);
      valueBaseConfig->addSendTo(config, reuse, skip);
      config->addBaseOn(valueBaseConfig, reuse, skip);
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
    config->addUsedBy(baseConfig);
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

void StreamFloatController::floatIndStreams(const Args &args) {
  if (!this->se->enableStreamFloatIndirect) {
    return;
  }
  while (true) {
    bool floated = false;
    for (auto dynS : args.dynStreams) {
      if (this->floatIndStream(args, dynS)) {
        floated = true;
      }
    }
    if (!floated) {
      break;
    }
  }

  this->fixMultiPredication(args);
}

int StreamFloatController::getFloatChainDepth(
    const CacheStreamConfigureData &config) const {
  for (const auto &baseEdge : config.baseEdges) {
    if (baseEdge.isUsedBy) {
      auto baseConfig = baseEdge.data.lock();
      if (!baseConfig) {
        DYN_S_PANIC(config.dynamicId, "Missing BaseConfig.");
      }
      return this->getFloatChainDepth(*baseConfig) + 1;
    }
  }
  return 0;
}

int StreamFloatController::getFloatChainChildrenSize(
    const CacheStreamConfigureData &config) const {

  auto numChildren = 0;
  for (const auto &depEdge : config.depEdges) {
    if (depEdge.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {
      numChildren += this->getFloatChainChildrenSize(*depEdge.data) + 1;
    }
  }
  return numChildren;
}

CacheStreamConfigureDataPtr StreamFloatController::getFloatRootConfig(
    CacheStreamConfigureDataPtr config) const {
  for (const auto &baseEdge : config->baseEdges) {
    if (baseEdge.isUsedBy) {
      auto baseConfig = baseEdge.data.lock();
      if (!baseConfig) {
        DYN_S_PANIC(config->dynamicId, "Missing BaseConfig.");
      }
      return this->getFloatRootConfig(baseConfig);
    }
  }
  // This is the root.
  return config;
}

bool StreamFloatController::isOnFloatChain(
    CacheStreamConfigureDataPtr chainEndConfig,
    const CacheStreamConfigureDataPtr &config) const {
  if (!chainEndConfig) {
    return false;
  }
  if (chainEndConfig == config) {
    return true;
  }
  for (const auto &baseEdge : chainEndConfig->baseEdges) {
    if (baseEdge.isUsedBy) {
      auto baseConfig = baseEdge.data.lock();
      if (!baseConfig) {
        DYN_S_PANIC(chainEndConfig->dynamicId, "Missing BaseConfig.");
      }
      return this->isOnFloatChain(baseConfig, config);
    }
  }
  return false;
}

bool StreamFloatController::floatIndStream(const Args &args, DynStream *dynS) {
  auto S = dynS->stream;
  auto &floatedMap = args.floatedMap;
  if (floatedMap.count(S)) {
    return false;
  }
  if (S->isDirectMemStream() || S->isPointerChase()) {
    return false;
  }
  if (!S->isLoadStream() && !S->isAtomicComputeStream() &&
      !S->isStoreComputeStream()) {
    return false;
  }
  StreamFloatPolicy::logS(*dynS) << "[FloatIndirect] >>>>>>>>> \n"
                                 << std::flush;
  const auto &addrBaseStreams = S->addrBaseStreams;
  /**
   * We assume:
   * 1. May have multiple AddrBaseMemS.
   * 2. All other AddrBaseS are IndVar streams with AffinePattern.
   *
   * To float:
   * 1. Float with the AddrBaseMemS with the max FloatChainDepth.
   * 2. Other AddrBaseMemS sends to our FloatRootS.
   * 3. UsedAffineIVS is recorded as special BaseEdge.
   *
   */
  std::vector<Stream *> addrBaseMemStreams;
  std::vector<Stream *> addrBaseIVAffineStreams;
  for (auto addrBaseS : addrBaseStreams) {
    if (addrBaseS->isPointerChaseIndVar()) {
      StreamFloatPolicy::logS(*dynS) << "[Not Float] AddrBasePtrChaseS "
                                     << addrBaseS->getStreamName() << ".\n"
                                     << std::flush;
      return false;
    }
    if (addrBaseS->isReduction()) {
      StreamFloatPolicy::logS(*dynS) << "[Not Float] AddrBaseReduceS "
                                     << addrBaseS->getStreamName() << ".\n"
                                     << std::flush;
      return false;
    }
    if (addrBaseS->isMemStream()) {
      if (!floatedMap.count(addrBaseS)) {
        StreamFloatPolicy::logS(*dynS) << "[Not Float] Unfloat AddrBaseMemS "
                                       << addrBaseS->getStreamName() << ".\n"
                                       << std::flush;
        return false;
      }
      addrBaseMemStreams.push_back(addrBaseS);
    } else {
      addrBaseIVAffineStreams.push_back(addrBaseS);
    }
  }

  // Check if all PredBaseStreams are floated.
  for (auto predBaseS : S->predBaseStreams) {
    if (!floatedMap.count(predBaseS)) {
      StreamFloatPolicy::logS(*dynS) << "[Not Float] Unfloat PredBaseS "
                                     << predBaseS->getStreamName() << ".\n"
                                     << std::flush;
      return false;
    }
  }

  // Check if all ValueBaseStreams are floated.
  for (auto valueBaseS : S->valueBaseStreams) {
    if (!floatedMap.count(valueBaseS)) {
      StreamFloatPolicy::logS(*dynS) << "[Not Float] Unfloat ValueBaseS "
                                     << valueBaseS->getStreamName() << ".\n"
                                     << std::flush;
      return false;
    }
  }

  /**
   * Check if there is an aliased StoreStream for this LoadStream, but
   * is not promoted into an UpdateStream.
   */
  StreamFloatPolicy::logS(*dynS)
      << "HasAliasedStore " << S->aliasBaseStream->hasAliasedStoreStream
      << " IsLoad " << S->isLoadStream() << " IsUpdate " << S->isUpdateStream()
      << ".\n"
      << std::flush;
  if (S->aliasBaseStream->hasAliasedStoreStream && S->isLoadStream() &&
      !S->isUpdateStream()) {
    StreamFloatPolicy::logS(*dynS) << "[Not Float] Aliased StoreS.\n"
                                   << std::flush;
    return false;
  }

  /**
   * Float with the Addr/PredBaseS with maximal float chain depth.
   */
  Stream *floatBaseS = nullptr;
  int floatBaseChainDepth = -1;
  bool floatBasePredBy = false;
  bool floatBasePredValue = false;
  for (auto baseS : addrBaseMemStreams) {
    const auto &config = *floatedMap.at(baseS);
    auto depth = this->getFloatChainDepth(config);
    if (depth > floatBaseChainDepth) {
      floatBaseChainDepth = depth;
      floatBaseS = baseS;
    }
  }
  for (auto baseS : S->predBaseStreams) {
    const auto &config = *floatedMap.at(baseS);
    auto depth = this->getFloatChainDepth(config);
    if (depth > floatBaseChainDepth) {
      floatBaseChainDepth = depth;
      floatBaseS = baseS;
      floatBasePredBy = true;
      floatBasePredValue = baseS->getPredValue(S->staticId);
    }
  }

  if (!floatBaseS) {
    StreamFloatPolicy::logS(*dynS) << "[Not Float] no FloatBaseS.\n"
                                   << std::flush;
    return false;
  }

  if (floatBaseChainDepth >= 1) {
    if (!this->se->myParams->enableFloatMultiLevelIndirectStoreCompute) {
      StreamFloatPolicy::logS(*dynS)
          << "[Not Float] Disable Multi-Level Indirect.\n"
          << std::flush;
      return false;
    }
  }

  auto floatBaseConfig = floatedMap.at(floatBaseS);
  // Only dependent on this direct stream.
  auto config =
      S->allocateCacheConfigureData(dynS->configSeqNum, true /* isIndirect */);
  auto floatBaseReuse = dynS->getBaseElemReuseCount(floatBaseS);
  floatBaseConfig->addUsedBy(config, floatBaseReuse, floatBasePredBy,
                             floatBasePredValue);

  // Get the FloatRootConfig.
  auto floatRootConfig = this->getFloatRootConfig(floatBaseConfig);
  auto floatRootS = floatRootConfig->stream;

  /**
   * Add UsedAffineIVS.
   */
  for (auto addrBaseIVAffineS : addrBaseIVAffineStreams) {
    this->addUsedAffineIV(config, dynS, addrBaseIVAffineS);
  }

  /**
   * For all the dependence other than FloatBaseS, we check:
   * 1. If it is from the same FloatChain, then we just add a BaseOn
   * edge, and assume LLC SE will automaically propagate the value
   * along the chain.
   * 2. Otherwise, we have to add a real SendTo edge to the FloatRootS.
   */

  auto addBaseS = [&](Stream *baseS, bool isPredBy) -> void {
    if (baseS == floatBaseS) {
      return;
    }
    auto &baseConfig = floatedMap.at(baseS);
    auto reuse = dynS->getBaseElemReuseCount(baseS);
    auto skip = dynS->getBaseElemSkipCount(baseS);
    if (isPredBy) {
      auto predValue = baseS->getPredValue(S->staticId);
      config->addPredBy(baseConfig, reuse, skip, predValue);
    } else {
      config->addBaseOn(baseConfig, reuse, skip);
    }

    if (this->isOnFloatChain(floatBaseConfig, baseConfig)) {
      // No need to add the SendTo edge.
      return;
    }

    /**
     * ValueBaseS will send to my AddrBaseS.
     * See the comment in CacheStreamConfigureData for the reuse/skip here.
     */
    auto sendReuse = reuse;
    auto sendSkip = skip;
    if (floatBaseReuse > 1) {
      assert(reuse == 1 && skip == 0 &&
             "Cannot have two reuses for IndS with ValueBaseS.");
      sendSkip = floatBaseReuse;
    }
    baseConfig->addSendTo(floatRootConfig, sendReuse, sendSkip);
  };

  for (auto valueBaseS : S->valueBaseStreams) {
    addBaseS(valueBaseS, false /* isPredBy */);
  }
  for (auto addrBaseMemS : addrBaseMemStreams) {
    addBaseS(addrBaseMemS, false /* isPredBy */);
  }
  for (auto predBaseS : S->predBaseStreams) {
    addBaseS(predBaseS, true /* isPredBy */);
  }

  DYN_S_DPRINTF(dynS->dynStreamId, "Offload as indirect Reuse %d.\n",
                floatBaseReuse);
  StreamFloatPolicy::logS(*dynS)
      << "[Float] IndS with Root " << floatRootS->getStreamName() << " Base "
      << floatBaseS->getStreamName() << " Depth " << floatBaseChainDepth
      << " Reuse " << floatBaseReuse << ".\n"
      << std::flush;
  floatedMap.emplace(S, config);
  if (S->getEnabledStoreFunc()) {
    if (!dynS->hasTotalTripCount()) {
      DYN_S_PANIC(dynS->dynStreamId,
                  "ComputeStream without TotalTripCount writes to memory.");
    }
  }
  return true;
}

void StreamFloatController::fixMultiPredication(const Args &args) {
  /**
   * We have encountered a special multi-predication in SSSP.
   *
   * weight, v = out_edge[i];
   * new_dist = u_dist + weight;
   * old_dist = __atomic_fetch_min(&dist[v], new_dist);
   * if (old_dist > new_dist) {
   *   Push to the bucket.
   * }
   *
   * Here the predication depends on two streams: out_edge[i] and dist[v],
   * while now in LLC we can only handle predication on a single stream.
   * However, here dist[v] is an indirect stream of out_edge[i]. Therefore,
   * we disable the predication from out_edge[i] and make sure predicated
   * streams only predicated by dist[v].
   *
   */
  auto &floatedMap = args.floatedMap;

  std::set<Stream *> finalPredBaseStreams;

  for (const auto &entry : floatedMap) {
    auto config = entry.second;

    // Collect all PredBaseStreams.
    std::vector<CacheStreamConfigureDataPtr> predBaseConfigs;
    for (const auto &baseE : config->baseEdges) {
      if (baseE.isPredBy) {
        auto baseConfig = baseE.data.lock();
        assert(baseConfig);
        predBaseConfigs.push_back(baseConfig);
      }
    }

    if (predBaseConfigs.empty()) {
      // No pred base.
      continue;
    }

    // Sort them with float chain depth.
    std::sort(predBaseConfigs.begin(), predBaseConfigs.end(),
              [this](const CacheStreamConfigureDataPtr &configA,
                     const CacheStreamConfigureDataPtr &configB) -> bool {
                return this->getFloatChainDepth(*configA) <
                       this->getFloatChainDepth(*configB);
              });

    /**
     * Some sanity check on the PredBaseS.
     *
     * 1. They should on the same float chain.
     * 2. They should have the same predicate callback.
     *
     */
    auto predBaseEndConfig = predBaseConfigs.back();
    assert(predBaseEndConfig->predCallback);

    finalPredBaseStreams.insert(predBaseEndConfig->stream);

    if (predBaseConfigs.size() <= 1) {
      // Not Multi-Predication.
      continue;
    }
    for (int i = 0; i + 1 < predBaseConfigs.size(); ++i) {
      auto baseConfig = predBaseConfigs.at(i);
      if (!this->isOnFloatChain(predBaseEndConfig, baseConfig)) {
        DYN_S_PANIC(config->dynamicId,
                    "[Multi-Pred] By different float chain: %s and %s.",
                    predBaseEndConfig->dynamicId, baseConfig->dynamicId);
      }
      if (!baseConfig->predCallback) {
        DYN_S_PANIC(baseConfig->dynamicId, "Missing PredCallback. Stream? %d.",
                    baseConfig->stream->getDynStream(baseConfig->dynamicId)
                        ->predCallback);
      }
      if (predBaseEndConfig->predCallback->getFuncInfo().name() !=
          baseConfig->predCallback->getFuncInfo().name()) {
        DYN_S_PANIC(config->dynamicId,
                    "[Multi-Pred] By different callback: %s and %s.",
                    predBaseEndConfig->dynamicId, baseConfig->dynamicId);
      }
    }

    // We can get rid of the PredByEdge except to the PredBaseEndConfig.
    std::vector<CacheStreamConfigureData::BaseEdge> newBaseEdges;
    for (const auto &baseE : config->baseEdges) {
      if (baseE.isPredBy) {
        auto baseConfig = baseE.data.lock();
        assert(baseConfig);
        if (baseConfig != predBaseEndConfig) {

          StreamFloatPolicy::logS(config->dynamicId)
              << "[Multi-Pred] Clear PredBy " << baseConfig->dynamicId << ".\n"
              << std::flush;
          continue;
        }
      }
      newBaseEdges.push_back(baseE);
    }

    config->baseEdges = newBaseEdges;
  }

  // Finally clear the PredCallback from those cleared PredS.
  for (const auto &entry : floatedMap) {
    auto config = entry.second;

    if (config->predCallback && !finalPredBaseStreams.count(config->stream)) {
      // Clear the PredCallback.
      config->predCallback = nullptr;
      config->predFormalParams.clear();
      StreamFloatPolicy::logS(config->dynamicId)
          << "[Multi-Pred] Clear PredCallback " << config->dynamicId << ".\n"
          << std::flush;
    }
  }
}

void StreamFloatController::floatDirectStoreComputeStreams(const Args &args) {
  auto &floatedMap = args.floatedMap;
  for (auto dynS : args.dynStreams) {
    auto S = dynS->stream;
    if (floatedMap.count(S)) {
      continue;
    }
    if (!S->isDirectMemStream()) {
      continue;
    }
    if (!S->isStoreComputeStream()) {
      continue;
    }
    this->floatDirectStoreComputeOrUpdateStream(args, dynS);
  }
}

void StreamFloatController::floatDirectUpdateStreams(const Args &args) {
  auto &floatedMap = args.floatedMap;
  for (auto dynS : args.dynStreams) {
    auto S = dynS->stream;
    if (floatedMap.count(S)) {
      continue;
    }
    if (!S->isDirectMemStream()) {
      continue;
    }
    if (!S->isUpdateStream()) {
      continue;
    }
    this->floatDirectStoreComputeOrUpdateStream(args, dynS);
  }
}

void StreamFloatController::floatDirectStoreComputeOrUpdateStream(
    const Args &args, DynStream *dynS) {
  auto &floatedMap = args.floatedMap;
  auto S = dynS->stream;
  auto floatDecision = this->policy->shouldFloatStream(*dynS);
  if (!floatDecision.shouldFloat) {
    return;
  }
  /**
   * Check for all the value base streams.
   */
  bool allValueBaseSFloated = true;
  for (auto valueBaseS : S->valueBaseStreams) {
    if (valueBaseS->isAffineIVStream()) {
      StreamFloatPolicy::logS(*dynS)
          << "Use AffineIVS " << valueBaseS->getStreamName() << "\n"
          << std::flush;
    }
    if (!floatedMap.count(valueBaseS)) {
      allValueBaseSFloated = false;
      StreamFloatPolicy::logS(*dynS) << "[Not Float] as UnFloated ValueBaseS "
                                     << valueBaseS->getStreamName() << "\n"
                                     << std::flush;
      break;
    }
  }
  if (!allValueBaseSFloated) {
    return;
  }
  // Add SendTo edge from value base streams to myself.
  S_DPRINTF(S, "Offload DirectStore/UpdateS.\n");
  auto config = S->allocateCacheConfigureData(dynS->configSeqNum);
  for (auto valueBaseS : S->valueBaseStreams) {
    if (valueBaseS->isAffineIVStream()) {
      this->addUsedAffineIV(config, dynS, valueBaseS);
      continue;
    }
    auto &valueBaseConfig = floatedMap.at(valueBaseS);
    auto reuse = dynS->getBaseElemReuseCount(valueBaseS);
    auto skip = dynS->getBaseElemSkipCount(valueBaseS);
    valueBaseConfig->addSendTo(config, reuse, skip);
    config->addBaseOn(valueBaseConfig, reuse, skip);
  }
  floatedMap.emplace(S, config);
  args.rootConfigVec.push_back(config);
}

void StreamFloatController::floatDirectOrPtrChaseReduceStreams(
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
    std::vector<Stream *> usedAffineIVStreams;
    for (auto backBaseS : S->backBaseStreams) {
      if (backBaseS == S) {
        continue;
      }
      if (backBaseS->isAffineIVStream()) {
        usedAffineIVStreams.push_back(backBaseS);
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
    int selectedBackBaseConfigIdx = 0;
    for (int i = 0; i < senderCount.size(); i++) {
      if (senderCount[i] > maxSenders) {
        maxSenders = senderCount[i];
        selectedBackBaseConfigIdx = i;
      }
    }

    /**
     * Special cases:
     * kmeans: with "feature.ld".
     * pointnet: with "mlp_inner.input.ld".
     * mm_inner_lnm: with "Bt.ld"
     * mm_inner_nlm: with "A.ld"
     */
    for (int i = 0; i < backBaseStreamConfigs.size(); ++i) {
      const auto &config = backBaseStreamConfigs.at(i);
      std::string strStreamName = config->dynamicId.streamName;
      bool found = false;
      if (strStreamName.find("gfm.kmeans.feature.ld") != std::string::npos) {
        found = true;
        selectedBackBaseConfigIdx = i;
      } else if (strStreamName.find("gfm.pointnet.mlp_inner.input.ld") !=
                 std::string::npos) {
        found = true;
        selectedBackBaseConfigIdx = i;
      } else if (strStreamName.find("gfm.mm_inner_lnm.Bt.ld") !=
                 std::string::npos) {
        found = true;
        selectedBackBaseConfigIdx = i;
      } else if (strStreamName.find("gfm.mm_inner_nlm.A.ld") !=
                 std::string::npos) {
        found = true;
        selectedBackBaseConfigIdx = i;
      }
      if (found) {
        S_DPRINTF(S, "ReduceS floated with pointnet %s.\n", config->dynamicId);
        break;
      }
    }

    // Make all others send to that one.
    auto &selectedBackBaseConfig =
        backBaseStreamConfigs.at(selectedBackBaseConfigIdx);
    selectedBackBaseConfig->addUsedBy(reductionConfig);
    S_DPRINTF(S, "ReduceS associated with %s, existing sender %d.\n",
              selectedBackBaseConfig->dynamicId, maxSenders);
    StreamFloatPolicy::logS(*dynS)
        << "[Float] as Reduction with " << selectedBackBaseConfig->dynamicId
        << ", existing # sender " << maxSenders << ".\n"
        << std::flush;
    for (int i = 0; i < backBaseStreamConfigs.size(); ++i) {
      if (i == selectedBackBaseConfigIdx) {
        continue;
      }
      auto &backBaseConfig = backBaseStreamConfigs.at(i);
      auto reuse = dynS->getBaseElemReuseCount(backBaseConfig->stream);
      auto skip = dynS->getBaseElemSkipCount(backBaseConfig->stream);
      backBaseConfig->addSendTo(selectedBackBaseConfig, reuse, skip);
      reductionConfig->addBaseOn(backBaseConfig, reuse, skip);
    }

    // Add all the UsedAffineIVS.
    for (auto usedAffineIVS : usedAffineIVStreams) {
      this->addUsedAffineIV(reductionConfig, dynS, usedAffineIVS);
    }
  }
}

void StreamFloatController::floatIndReduceStream(const Args &args,
                                                 DynStream *dynS) {
  auto &floatedMap = args.floatedMap;
  auto S = dynS->stream;
  /**
   * IndirectReductionStream:
   * 1. Only use one floated Ind/Reduce stream.
   * 2. The ReductionFunc is some simple operation.
   * 3. The total trip count is long enough.
   */
  if (floatedMap.count(S) || !S->isReduction() || !S->addrBaseStreams.empty()) {
    return;
  }
  Stream *backBaseIndS = nullptr;
  std::vector<Stream *> backBaseStreams;
  std::vector<Stream *> backBaseIVAffineStreams;
  std::vector<Stream *> backBaseDirectStreams;
  for (auto BS : S->backBaseStreams) {
    if (BS == S) {
      // Ignore dependence on myself.
      continue;
    }
    if (BS->isIndirectLoadStream() || BS->isReduction()) {
      if (backBaseIndS) {
        StreamFloatPolicy::logS(*dynS)
            << "[Not Float] as IndReduce with multi BackBaseIndS.\n"
            << std::flush;
        return;
      }
      backBaseIndS = BS;
      backBaseStreams.push_back(BS);
    } else if (BS->isDirectLoadStream()) {
      backBaseStreams.push_back(BS);
      backBaseDirectStreams.push_back(BS);
    } else {
      backBaseIVAffineStreams.push_back(BS);
    }
  }
  if (!backBaseIndS) {
    S_DPRINTF(S, "Not an IndReduceS to float.");
    return;
  }
  if (!this->se->myParams->enableFloatIndirectReduction) {
    StreamFloatPolicy::logS(*dynS) << "[Not Float] as IndReduce is disabled.\n"
                                   << std::flush;
    return;
  }
  for (auto BS : backBaseStreams) {
    if (!floatedMap.count(BS)) {
      StreamFloatPolicy::logS(*dynS) << "[Not Float] as unfloated BackBaseS "
                                     << BS->getStreamName() << ".\n"
                                     << std::flush;
      return;
    }
  }

  // Check if my reduction is simple operation.
  if (!S->isReductionDistributable()) {
    StreamFloatPolicy::logS(*dynS)
        << "[Not Float] as undistributable op TripCount "
        << dynS->getTotalTripCount() << ".\n"
        << std::flush;
    return;
  }
  auto &backBaseIndConfig = floatedMap.at(backBaseIndS);
  // Check the total trip count.
  uint64_t floatIndReduceMinTripCount = 256;
  if (!dynS->hasTotalTripCount() ||
      dynS->getTotalTripCount() < floatIndReduceMinTripCount) {
  }
  /**
   * Okay now we decided to float the IndReduceS.
   */
  auto reduceConfig = S->allocateCacheConfigureData(dynS->configSeqNum, true);
  // Reduction stream is always one iteration behind.
  reduceConfig->isOneIterationBehind = true;
  dynS->setFloatedOneIterBehind(true);
  floatedMap.emplace(S, reduceConfig);

  // Add UsedBy edge to the BackBaseIndirectS.
  backBaseIndConfig->addUsedBy(reduceConfig);
  S_DPRINTF(S, "Float IndReduceS associated with %s.\n",
            backBaseIndConfig->dynamicId);
  StreamFloatPolicy::logS(*dynS)
      << "[Float] as IndReduce with " << backBaseIndConfig->dynamicId
      << " TotalTripCount " << dynS->getTotalTripCount() << ".\n"
      << std::flush;

  auto floatRootConfig = this->getFloatRootConfig(backBaseIndConfig);

  for (auto backBaseDirectS : backBaseDirectStreams) {
    /**
     * As a trick, we add a SendTo edge from BackBaseDirectS to floatRootS
     * with the IndReduceS as the receiver.
     * NOTE: No need to send from floatRootS.
     */
    auto &backBaseDirectConfig = floatedMap.at(backBaseDirectS);
    auto reuse = dynS->getBaseElemReuseCount(backBaseDirectConfig->stream);
    auto skip = dynS->getBaseElemSkipCount(backBaseDirectConfig->stream);
    if (backBaseDirectS != floatRootConfig->stream) {
      backBaseDirectConfig->addSendTo(floatRootConfig, reuse, skip);
    }
    reduceConfig->addBaseOn(backBaseDirectConfig, reuse, skip);
    StreamFloatPolicy::logS(*dynS)
        << "[Float] as IndReduce with BackBaseDirectS "
        << backBaseDirectConfig->dynamicId << ".\n"
        << std::flush;
  }

  /**
   * Add UsedAffineIVS.
   */
  for (auto backBaseIVAffineS : backBaseIVAffineStreams) {
    this->addUsedAffineIV(reduceConfig, dynS, backBaseIVAffineS);
    StreamFloatPolicy::logS(*dynS)
        << "[Float] as IndReduce with BackBaseIVAffineS "
        << backBaseIVAffineS->getStreamName() << ".\n"
        << std::flush;
  }
}

void StreamFloatController::floatIndirectReductionStreams(const Args &args) {
  for (auto dynS : args.dynStreams) {
    this->floatIndReduceStream(args, dynS);
  }
}

void StreamFloatController::floatMultiLevelIndirectStoreComputeStreams(
    const Args &args) {
  if (!this->se->myParams->enableFloatMultiLevelIndirectStoreCompute) {
    return;
  }
  for (auto dynS : args.dynStreams) {
    this->floatMultiLevelIndirectStoreComputeStream(args, dynS);
  }
}

void StreamFloatController::floatMultiLevelIndirectStoreComputeStream(
    const Args &args, DynStream *dynS) {
  auto &floatedMap = args.floatedMap;
  auto S = dynS->stream;
  if (!S->isStoreComputeStream()) {
    return;
  }
  if (S->isDirectMemStream()) {
    return;
  }
  /**
   * We can float it iff.
   * 1. All AddrBaseStreams are floated.
   * 2. All AddrBaseStreams formed an float chain.
   *
   * Then we float the stream by extend the float chain,
   * and let other AddrBaseStream forward to it.
   */
  for (auto addrBaseS : S->addrBaseStreams) {
    if (!floatedMap.count(addrBaseS)) {
      return;
    }
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
  addrBaseConfig->addUsedBy(myConfig);

  StreamFloatPolicy::logS(*dynS)
      << "[Float] as Two-Level IndirectStoreCompute associated with "
      << addrBaseConfig->dynamicId << "\n"
      << std::flush;

  // Add special SendTo edge from AddrRootS to itself if needed.
  for (auto valueBaseS : S->valueBaseStreams) {
    if (valueBaseS == addrRootS) {
      auto reuse = dynS->getBaseElemReuseCount(valueBaseS);
      auto skip = dynS->getBaseElemSkipCount(valueBaseS);
      addrRootConfig->addSendTo(addrRootConfig, reuse, skip);
      myConfig->addBaseOn(addrRootConfig, reuse, skip);
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

  auto &dynBound = dynRegion.loopBound;
  auto &staticBound = dynRegion.staticRegion->loopBound;
  if (!staticBound.boundFunc) {
    return;
  }

  /**
   * Check that LoopBound is dependent on one MemStream and maybe UsedAffineIVS.
   */
  std::vector<DynStream *> boundBaseMemStreams;
  std::vector<DynStream *> boundBaseIVAffineStreams;
  for (auto S : staticBound.baseStreams) {
    auto &dynS = S->getDynStream(args.seqNum);
    if (S->isPointerChaseIndVar()) {
      StreamFloatPolicy::logS(dynS) << "[Not Float] LoopBound: BasePtrChaseS.\n"
                                    << std::flush;
      return;
    }
    if (S->isReduction()) {
      StreamFloatPolicy::logS(dynS) << "[Not Float] LoopBound: BaseReduceS.\n"
                                    << std::flush;
      return;
    }
    if (S->isMemStream()) {
      if (!args.floatedMap.count(S)) {
        StreamFloatPolicy::logS(dynS)
            << "[Not Float] LoopBound: Unfloated BaseMemS.\n"
            << std::flush;
        return;
      }
      boundBaseMemStreams.push_back(&dynS);
    } else {
      boundBaseIVAffineStreams.push_back(&dynS);
    }
  }

  // For now, let's just support single BaseStream.
  StreamFloatPolicy::getLog() << "[LoopBound] BaseMemS: ";
  for (auto dynS : boundBaseMemStreams) {
    StreamFloatPolicy::logS(*dynS) << ' ';
  }
  StreamFloatPolicy::getLog() << '\n' << std::flush;
  StreamFloatPolicy::getLog() << "[LoopBound] BaseIVAffineS: ";
  for (auto dynS : boundBaseIVAffineStreams) {
    StreamFloatPolicy::logS(*dynS) << ' ';
  }
  StreamFloatPolicy::getLog() << '\n' << std::flush;

  if (boundBaseMemStreams.size() != 1) {
    StreamFloatPolicy::getLog()
        << "[Not Float] LoopBound: Not Single BaseMemS.\n"
        << std::flush;
    return;
  }

  auto &baseDynS = *boundBaseMemStreams.front();
  auto baseS = baseDynS.stream;

  // Enforce that BaseIVAffineS is from the same loop.
  for (auto baseIVAffineDynS : boundBaseIVAffineStreams) {
    auto baseIVAffineS = baseIVAffineDynS->stream;
    if (baseIVAffineS->getLoopLevel() != baseS->getLoopLevel() ||
        baseIVAffineS->getConfigLoopLevel() != baseS->getConfigLoopLevel()) {
      StreamFloatPolicy::logS(baseDynS)
          << "[Not Float] LoopBound: Illegal BaseIVAffineS "
          << baseIVAffineDynS->dynStreamId << "\n"
          << std::flush;
      return;
    }
  }

  auto &baseConfig = args.floatedMap.at(baseS);
  baseConfig->loopBoundFormalParams = dynBound.formalParams;
  baseConfig->loopBoundCallback = dynBound.boundFunc;
  baseConfig->loopBoundRet = staticBound.boundRet;

  // Add the BaseAffineIVS as special edge and simple reuse/skip.
  for (auto baseIVAffineDynS : boundBaseIVAffineStreams) {
    int reuse = 1;
    int skip = 0;
    this->addUsedAffineIVWithReuseSkip(baseConfig, &baseDynS,
                                       baseIVAffineDynS->stream, reuse, skip);
  }

  // Remember that this bound is offloaded.
  dynBound.offloaded = true;
  StreamFloatPolicy::logS(baseDynS)
      << "[Float] LoopBound for " << args.region.region() << ".\n"
      << std::flush;
  SE_DPRINTF("[LoopBound] Offloaded LoopBound for %s.\n", args.region.region());
}

void StreamFloatController::decideMLCBufferNumSlices(const Args &args) {
  for (auto &config : args.rootConfigVec) {
    auto S = config->stream;
    auto dynS = S->getDynStream(config->dynamicId);

    config->mlcBufferNumSlices =
        this->se->myParams->mlc_stream_buffer_init_num_entries;
    if (this->se->myParams->mlc_stream_buffer_init_num_entries == 0) {
      config->mlcBufferNumSlices = std::min(S->maxSize, 32ul);
    }

    /**
     * We observe that allowing too many credits for streams with indirect
     * streams is bad for performance. Here we limit that if it contains
     * more than 1 indirect streams, e.g. bfs_push, bfs_pull.
     */
    auto numChildren = this->getFloatChainChildrenSize(*config);
    if (numChildren > 1) {
      config->mlcBufferNumSlices =
          this->se->myParams->mlc_ind_stream_buffer_init_num_entries;
    }

    StreamFloatPolicy::logS(*dynS) << "[MLCBuffer] MLCBufferNumSlices "
                                   << config->mlcBufferNumSlices << ".\n"
                                   << std::flush;
    DYN_S_DPRINTF(dynS->dynStreamId, "[MLCBuffer] MLCBufferNumSlices %llu.\n",
                  config->mlcBufferNumSlices);
  }
}

void StreamFloatController::setLoopBoundFirstOffloadedElemIdx(
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
    dynS->updateFloatInfoForElems();
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
  auto firstFloatElemIdx = config->floatPlan.getFirstFloatElementIdx();
  if (dynS->FIFOIdx.entryIdx <= firstFloatElemIdx) {
    DYN_S_DPRINTF(dynS->dynStreamId,
                  "[MidwayFloat] DynS TailElem %llu < FirstFloatElem %llu. "
                  "Not Yet Float.\n",
                  dynS->FIFOIdx.entryIdx, firstFloatElemIdx);
    return false;
  }
  /**
   * If there is StreamLoopBound, also check that LoopBound has evaluated.
   */
  auto &dynRegion = this->se->regionController->getDynRegion(
      "[MidwayFloat] Check LoopBound before MidwayFloat", dynS->configSeqNum);
  if (dynRegion.staticRegion->region.is_loop_bound()) {
    auto &dynBound = dynRegion.loopBound;
    if (dynBound.nextElemIdx > firstFloatElemIdx) {
      DYN_S_PANIC(dynS->dynStreamId,
                  "[MidwayFloat] Impossible! LoopBound NextElem %llu > "
                  "FirstFloatElem %llu.",
                  dynBound.nextElemIdx, firstFloatElemIdx);
    }
    if (dynBound.nextElemIdx < firstFloatElemIdx) {
      DYN_S_DPRINTF(dynS->dynStreamId,
                    "[MidwayFloat] LoopBound NextElem %llu < FirstFloatElem "
                    "%llu. Not Yet Float.\n",
                    dynBound.nextElemIdx, firstFloatElemIdx);
      return false;
    }
    if (dynBound.brokenOut) {
      DYN_S_DPRINTF(dynS->dynStreamId,
                    "[MidwayFloat] LoopBound BrokenOut NextElem %llu <= "
                    "FirstFloatElem %llu. Don't Float.\n",
                    dynBound.nextElemIdx, firstFloatElemIdx);
      return false;
    }
  }
  if (S->isReduction() || S->isPointerChaseIndVar()) {
    /**
     * Since these streams are one iteration behind, we require them to be value
     * ready.
     */
    auto firstFloatElem = dynS->getElemByIdx(firstFloatElemIdx);
    if (!firstFloatElem) {
      DYN_S_PANIC(dynS->dynStreamId,
                  "[MidwayFloat] FirstFloatElem already released.");
    }
    if (!firstFloatElem->isValueReady) {
      S_ELEMENT_DPRINTF(
          firstFloatElem,
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

void StreamFloatController::addUsedAffineIV(CacheStreamConfigureDataPtr &config,
                                            DynStream *dynS,
                                            Stream *affineIVS) {

  auto reuse = dynS->getBaseElemReuseCount(affineIVS);
  auto skip = dynS->getBaseElemSkipCount(affineIVS);
  this->addUsedAffineIVWithReuseSkip(config, dynS, affineIVS, reuse, skip);
}

void StreamFloatController::addUsedAffineIVWithReuseSkip(
    CacheStreamConfigureDataPtr &config, DynStream *dynS, Stream *affineIVS,
    int reuse, int skip) {
  auto affineIVConfig =
      affineIVS->allocateCacheConfigureDataForAffineIV(dynS->configSeqNum);
  DYN_S_DPRINTF(dynS->dynStreamId, "Add AffineIV %s R/S %d/%d.\n",
                affineIVS->streamName, reuse, skip);
  config->addBaseAffineIV(affineIVConfig, reuse, skip);
}
} // namespace gem5
