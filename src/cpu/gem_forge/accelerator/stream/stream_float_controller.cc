#include "stream_float_controller.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamEngine, format, ##args)

#include "debug/CoreRubyStreamLife.hh"
#include "debug/StreamEngine.hh"
#define DEBUG_TYPE StreamEngine
#include "stream_log.hh"

StreamFloatController::StreamFloatController(
    StreamEngine *_se, std::unique_ptr<StreamFloatPolicy> _policy)
    : se(_se), policy(std::move(_policy)) {}

void StreamFloatController::floatStreams(
    const StreamConfigArgs &args, const ::LLVM::TDG::StreamRegion &region,
    std::list<DynamicStream *> &dynStreams) {

  if (this->se->cpuDelegator->cpuType ==
      GemForgeCPUDelegator::CPUTypeE::ATOMIC_SIMPLE) {
    SE_DPRINTF("Skip StreamFloat in AtomicSimpleCPU for %s.\n",
               region.region());
    return;
  }

  auto *cacheStreamConfigVec = new CacheStreamConfigureVec();
  StreamCacheConfigMap offloadedStreamConfigMap;
  SE_DPRINTF("Consider StreamFloat for %s.\n", region.region());

  // /**
  //  * Floating decisions are made in multiple phases, first affine, then
  //  others.
  //  */
  // for (auto dynS : dynStreams) {
  //   /**
  //    * StreamAwareCache: Send a StreamConfigReq to the cache hierarchy.
  //    * TODO: Rewrite this bunch of hack.
  //    */
  //   auto S = dynS->stream;
  //   if (offloadedStreamConfigMap.count(S)) {
  //     continue;
  //   }
  //   if (this->policy->shouldFloatStream(*dynS)) {

  //     // Get the CacheStreamConfigureData.
  //     auto streamConfigureData =
  //         S->allocateCacheConfigureData(dynS->configSeqNum);

  //     // Remember the offloaded decision.
  //     dynS->offloadedToCacheAsRoot = true;
  //     dynS->offloadedToCache = true;
  //     this->se->numFloated++;
  //     offloadedStreamConfigMap.emplace(S, streamConfigureData);

  //     // Remember the pseudo offloaded decision.
  //     if (this->se->enableStreamFloatPseudo &&
  //         this->policy->shouldPseudoFloatStream(*dynS)) {
  //       dynS->pseudoOffloadedToCache = true;
  //       streamConfigureData->isPseudoOffload = true;
  //     }

  //     if (S->isPointerChaseLoadStream()) {
  //       streamConfigureData->isPointerChase = true;
  //     }

  //     /**
  //      * If we enable these indirect streams to float:
  //      * 1. LoadStream.
  //      * 2. Store/AtomicRMWStream with StoreFunc enabled, and has not been
  //      * merged.
  //      */
  //     if (this->se->enableStreamFloatIndirect) {
  //       for (auto depS : S->addrDepStreams) {
  //         bool canFloatIndirect = false;
  //         auto depSType = depS->getStreamType();
  //         switch (depSType) {
  //         case ::LLVM::TDG::StreamInfo_Type_LD:
  //           canFloatIndirect = true;
  //           break;
  //         case ::LLVM::TDG::StreamInfo_Type_AT:
  //         case ::LLVM::TDG::StreamInfo_Type_ST:
  //           if (depS->getEnabledStoreFunc() &&
  //               !depS->isMergedLoadStoreDepStream()) {
  //             canFloatIndirect = true;
  //           }
  //           break;
  //         default:
  //           break;
  //         }
  //         if (canFloatIndirect && depS->addrBaseStreams.size() == 1) {
  //           // Only dependent on this direct stream.
  //           auto depConfig = depS->allocateCacheConfigureData(
  //               dynS->configSeqNum, true /* isIndirect */);
  //           streamConfigureData->addUsedBy(depConfig);
  //           // Remember the decision.
  //           auto &depDynS = depS->getDynamicStream(dynS->configSeqNum);
  //           depDynS.offloadedToCache = true;
  //           this->se->numFloated++;
  //           S_DPRINTF(depS, "Offload as indirect.\n");
  //           assert(offloadedStreamConfigMap.emplace(depS, depConfig).second
  //           &&
  //                  "Already offloaded this indirect stream.");
  //           // ! Pure hack here to indclude merged stream of this indirect
  //           // ! stream.
  //           for (auto mergedStreamId : depS->getMergedPredicatedStreams()) {
  //             auto mergedS = this->se->getStream(mergedStreamId.id().id());
  //             auto mergedConfig = mergedS->allocateCacheConfigureData(
  //                 dynS->configSeqNum, true /* isIndirect */);
  //             mergedConfig->isPredicated = true;
  //             mergedConfig->isPredicatedTrue = mergedStreamId.pred_true();
  //             mergedConfig->predicateStreamId = depDynS.dynamicStreamId;
  //             /**
  //              * Remember the decision.
  //              */
  //             mergedS->getDynamicStream(dynS->configSeqNum).offloadedToCache
  //             =
  //                 true;
  //             this->se->numFloated++;
  //             assert(offloadedStreamConfigMap.emplace(mergedS, mergedConfig)
  //                        .second &&
  //                    "Merged stream already offloaded.");
  //             streamConfigureData->addUsedBy(mergedConfig);
  //           }
  //         }
  //       }
  //       // ! Disable one iteration behind indirect streams so far.
  //       // if (streamConfigureData->indirectStreams.empty()) {
  //       //   // Not found a valid indirect stream, let's try to search for
  //       //   // a indirect stream that is one iteration behind.
  //       //   for (auto backDependentStream : S->backDepStreams) {
  //       //     if (backDependentStream->getStreamType() != "phi") {
  //       //       continue;
  //       //     }
  //       //     if (backDependentStream->backBaseStreams.size() != 1) {
  //       //       continue;
  //       //     }
  //       //     for (auto indirectStream :
  //       backDependentStream->addrDepStreams)
  //       //     {
  //       //       if (indirectStream == S) {
  //       //         continue;
  //       //       }
  //       //       if (indirectStream->getStreamType() != "load") {
  //       //         continue;
  //       //       }
  //       //       if (indirectStream->addrBaseStreams.size() != 1) {
  //       //         continue;
  //       //       }
  //       //       // We found one valid indirect stream that is one iteration
  //       //       // behind S.
  //       //       streamConfigureData->addUsedBy(
  //       // indirectStream->allocateCacheConfigureData(args.seqNum));
  //       //       streamConfigureData->indirectStreams.back()
  //       //           ->isOneIterationBehind = true;
  //       //       break;
  //       //     }
  //       //     if (!streamConfigureData->indirectStreams.empty()) {
  //       //       // We already found one.
  //       //       break;
  //       //     }
  //       //   }
  //       // }
  //     }

  //     /**
  //      * Merged predicated streams are always offloaded.
  //      */
  //     for (auto mergedStreamId : S->getMergedPredicatedStreams()) {
  //       auto mergedS = this->se->getStream(mergedStreamId.id().id());
  //       auto mergedConfig = mergedS->allocateCacheConfigureData(
  //           dynS->configSeqNum, true /* isIndirect */);
  //       mergedConfig->isPredicated = true;
  //       mergedConfig->isPredicatedTrue = mergedStreamId.pred_true();
  //       mergedConfig->predicateStreamId = dynS->dynamicStreamId;
  //       /**
  //        * Remember the decision.
  //        */
  //       mergedS->getDynamicStream(dynS->configSeqNum).offloadedToCache =
  //       true; this->se->numFloated++;
  //       assert(offloadedStreamConfigMap.emplace(mergedS, mergedConfig).second
  //       &&
  //              "Merged stream already offloaded.");
  //       streamConfigureData->addUsedBy(mergedConfig);
  //     }
  //     /**
  //      * ValueDepStreams are always offloaded.
  //      */
  //     int numOffloadedValueDepStreams = 0;
  //     for (auto valueDepS : S->valueDepStreams) {
  //       auto valueDepConfig = valueDepS->allocateCacheConfigureData(
  //           dynS->configSeqNum, true /* isIndirect */);
  //       /**
  //        * Remember the decision.
  //        */
  //       valueDepS->getDynamicStream(dynS->configSeqNum).offloadedToCache =
  //       true; this->se->numFloated++;
  //       assert(offloadedStreamConfigMap.emplace(valueDepS, valueDepConfig)
  //                  .second &&
  //              "ValueDepStream already offloaded.");
  //       streamConfigureData->addUsedBy(valueDepConfig);
  //       numOffloadedValueDepStreams++;
  //     }

  //     cacheStreamConfigVec->push_back(streamConfigureData);
  //   }
  // }

  /**
   * This is our new float decision scheme in the following order.
   * - DirectLoadStream.
   * - IndirectLoadStream.
   * - DirectStoreComputeStream/UpdateStream.
   * - ReductionStreams.
   */
  Args floatArgs(region, dynStreams, offloadedStreamConfigMap,
                 *cacheStreamConfigVec);
  this->floatDirectLoadStreams(floatArgs);
  this->floatDirectAtomicComputeStreams(floatArgs);
  this->floatIndirectStreams(floatArgs);
  this->floatDirectStoreComputeOrUpdateStreams(floatArgs);
  this->floatDirectReductionStreams(floatArgs);
  this->floatIndirectReductionStreams(floatArgs);
  this->floatTwoLevelIndirectStoreComputeStreams(floatArgs);

  // Sanity check for some offload decision.
  bool hasOffloadStoreFunc = false;
  for (auto &dynS : dynStreams) {
    auto S = dynS->stream;
    if (dynS->offloadedToCache) {
      if (S->getEnabledStoreFunc()) {
        hasOffloadStoreFunc = true;
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

  // Send all the floating streams in one packet.
  // Dummy paddr to make ruby happy.
  Addr initPAddr = 0;
  auto pkt = GemForgePacketHandler::createStreamControlPacket(
      initPAddr, this->se->cpuDelegator->dataMasterId(), 0,
      MemCmd::Command::StreamConfigReq,
      reinterpret_cast<uint64_t>(cacheStreamConfigVec));
  if (hasOffloadStoreFunc) {
    // We have to delay this float config until StreamConfig is committed,
    // as so far we have no way to rewind the offloaded writes.
    this->configSeqNumToDelayedFloatPktMap.emplace(args.seqNum, pkt);
    for (auto &dynS : dynStreams) {
      if (dynS->offloadedToCache) {
        DYN_S_DPRINTF(dynS->dynamicStreamId, "Delayed FloatConfig.\n");
        dynS->offloadConfigDelayed = true;
      }
    }
  } else {
    for (auto &dynS : dynStreams) {
      if (dynS->offloadedToCache) {
        DYN_S_DPRINTF_(CoreRubyStreamLife, dynS->dynamicStreamId,
                       "Send FloatConfig.\n");
      }
    }
    this->se->cpuDelegator->sendRequest(pkt);
  }
}

void StreamFloatController::commitFloatStreams(const StreamConfigArgs &args,
                                               const StreamList &streams) {
  // We can issue the delayed float configuration now.
  auto iter = this->configSeqNumToDelayedFloatPktMap.find(args.seqNum);
  if (iter != this->configSeqNumToDelayedFloatPktMap.end()) {
    auto pkt = iter->second;
    for (auto S : streams) {
      auto &dynS = S->getDynamicStream(args.seqNum);
      if (dynS.offloadedToCache) {
        assert(dynS.offloadConfigDelayed && "Offload is not delayed.");
        dynS.offloadConfigDelayed = false;
        DYN_S_DPRINTF_(CoreRubyStreamLife, dynS.dynamicStreamId,
                       "Send Delayed FloatConfig.\n");
        DYN_S_DPRINTF(dynS.dynamicStreamId, "Send Delayed FloatConfig.\n");
      }
    }
    this->se->cpuDelegator->sendRequest(pkt);
    this->configSeqNumToDelayedFloatPktMap.erase(iter);
  }
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
    if (dynS.offloadedToCache) {
      // Sanity check that we don't break semantics.
      DYN_S_DPRINTF(dynS.dynamicStreamId, "Rewind floated stream.\n");
      if ((S->isAtomicComputeStream() || S->isStoreComputeStream()) &&
          !dynS.offloadConfigDelayed) {
        DYN_S_PANIC(dynS.dynamicStreamId,
                    "Rewind a floated Atomic/StoreCompute stream.");
      }
      if (dynS.offloadedToCacheAsRoot && !dynS.offloadConfigDelayed) {
        floatedIds.push_back(dynS.dynamicStreamId);
      }
      S->statistic.numFloatRewinded++;
      dynS.offloadedToCache = false;
      dynS.offloadConfigDelayed = false;
      dynS.offloadedToCacheAsRoot = false;
    }
  }
  if (!floatedIds.empty()) {
    this->se->sendStreamFloatEndPacket(floatedIds);
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
    if (!this->policy->shouldFloatStream(*dynS)) {
      continue;
    }
    // Additional check TotalTripCount is not 0. This is only for NestStream.
    if (dynS->hasTotalTripCount() && dynS->getTotalTripCount() == 0) {
      continue;
    }

    // Get the CacheStreamConfigureData.
    auto config = S->allocateCacheConfigureData(dynS->configSeqNum);

    // Remember the offloaded decision.
    dynS->offloadedToCacheAsRoot = true;
    dynS->offloadedToCache = true;
    this->se->numFloated++;
    floatedMap.emplace(S, config);
    args.rootConfigVec.push_back(config);

    // Remember the pseudo offloaded decision.
    if (this->se->enableStreamFloatPseudo &&
        this->policy->shouldPseudoFloatStream(*dynS)) {
      dynS->pseudoOffloadedToCache = true;
      config->isPseudoOffload = true;
    }

    if (S->isPointerChaseLoadStream()) {
      config->isPointerChase = true;
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
    if (!this->policy->shouldFloatStream(*dynS)) {
      continue;
    }

    // Get the CacheStreamConfigureData.
    auto config = S->allocateCacheConfigureData(dynS->configSeqNum);

    // Remember the offloaded decision.
    dynS->offloadedToCacheAsRoot = true;
    dynS->offloadedToCache = true;
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
      dynS->pseudoOffloadedToCache = true;
      config->isPseudoOffload = true;
    }

    if (S->isPointerChaseLoadStream()) {
      config->isPointerChase = true;
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
    if (S->isDirectMemStream()) {
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
    dynS->offloadedToCache = true;
    this->se->numFloated++;
    DYN_S_DPRINTF(dynS->dynamicStreamId, "Offload as indirect.\n");
    StreamFloatPolicy::logStream(S) << "[Float] as indirect.\n" << std::flush;
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
    if (!this->policy->shouldFloatStream(*dynS)) {
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
    dynS->offloadedToCache = true;
    dynS->offloadedToCacheAsRoot = true;
    this->se->numFloated++;
    S_DPRINTF(S, "Offload DirectStoreStream.\n");
    floatedMap.emplace(S, config);
    args.rootConfigVec.push_back(config);
  }
}

void StreamFloatController::floatDirectReductionStreams(const Args &args) {
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
    bool allBackBaseStreamsAreAffine = true;
    bool allBackBaseStreamsAreFloated = true;
    std::vector<CacheStreamConfigureDataPtr> backBaseStreamConfigs;
    for (auto backBaseS : S->backBaseStreams) {
      if (backBaseS == S) {
        continue;
      }
      if (!backBaseS->isDirectMemStream()) {
        allBackBaseStreamsAreAffine = false;
        break;
      }
      if (!floatedMap.count(backBaseS)) {
        allBackBaseStreamsAreFloated = false;
        break;
      }
      backBaseStreamConfigs.emplace_back(floatedMap.at(backBaseS));
    }
    if (!allBackBaseStreamsAreAffine) {
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
     * Okay now we decided to float the ReductionStream. We pick the
     * affine BackBaseStream A that has most SendTo edges and make
     * all other BackBaseStreams send to that stream.
     */
    auto reductionConfig =
        S->allocateCacheConfigureData(dynS->configSeqNum, true);
    // Reduction stream is always one iteration behind.
    reductionConfig->isOneIterationBehind = true;
    dynS->offloadedToCache = true;
    this->se->numFloated++;
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
    StreamFloatPolicy::logStream(S)
        << "[Float] as Reduction with " << baseConfigWithMostSenders->dynamicId
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
  dynS->offloadedToCache = true;
  this->se->numFloated++;
  floatedMap.emplace(S, reductionConfig);

  // Add UsedBy edge to the BackBaseIndirectS.
  backBaseIndirectConfig->addUsedBy(reductionConfig);
  S_DPRINTF(S, "Float IndirectReductionStream associated with %s.\n",
            backBaseIndirectConfig->dynamicId);
  StreamFloatPolicy::logStream(S)
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
  dynS->offloadedToCache = true;
  this->se->numFloated++;
  floatedMap.emplace(S, myConfig);
  addrBaseConfig->addUsedBy(myConfig);

  StreamFloatPolicy::logStream(S)
      << "[Float] as Two-Level IndirectStoreCompute associated with "
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