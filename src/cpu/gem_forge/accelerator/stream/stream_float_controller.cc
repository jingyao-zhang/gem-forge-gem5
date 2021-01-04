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
    const StreamConfigArgs &args, const ::LLVM::TDG::StreamRegion &streamRegion,
    std::list<Stream *> &configStreams) {

  if (this->se->cpuDelegator->cpuType ==
      GemForgeCPUDelegator::CPUTypeE::ATOMIC_SIMPLE) {
    SE_DPRINTF("Skip StreamFloat in AtomicSimpleCPU for %s.\n",
               streamRegion.region());
    return;
  }

  auto *cacheStreamConfigVec = new CacheStreamConfigureVec();
  StreamCacheConfigMap offloadedStreamConfigMap;
  SE_DPRINTF("Consider StreamFloat for %s.\n", streamRegion.region());

  /**
   * Floating decisions are made in multiple phases, first affine, then others.
   */
  for (auto &S : configStreams) {
    /**
     * StreamAwareCache: Send a StreamConfigReq to the cache hierarchy.
     * TODO: Rewrite this bunch of hack.
     */
    if (offloadedStreamConfigMap.count(S)) {
      continue;
    }
    auto &dynStream = S->getDynamicStream(args.seqNum);
    if (this->policy->shouldFloatStream(S, dynStream)) {

      // Get the CacheStreamConfigureData.
      auto streamConfigureData = S->allocateCacheConfigureData(args.seqNum);

      // Remember the offloaded decision.
      dynStream.offloadedToCacheAsRoot = true;
      dynStream.offloadedToCache = true;
      this->se->numFloated++;
      offloadedStreamConfigMap.emplace(S, streamConfigureData);

      // Remember the pseudo offloaded decision.
      if (this->se->enableStreamFloatPseudo &&
          this->policy->shouldPseudoFloatStream(S, dynStream)) {
        dynStream.pseudoOffloadedToCache = true;
        streamConfigureData->isPseudoOffload = true;
      }

      if (S->isPointerChaseLoadStream()) {
        streamConfigureData->isPointerChase = true;
      }

      /**
       * If we enable these indirect streams to float:
       * 1. LoadStream.
       * 2. Store/AtomicRMWStream with StoreFunc enabled, and has not been
       * merged.
       */
      if (this->se->enableStreamFloatIndirect) {
        for (auto depS : S->addrDepStreams) {
          bool canFloatIndirect = false;
          auto depSType = depS->getStreamType();
          switch (depSType) {
          case ::LLVM::TDG::StreamInfo_Type_LD:
            canFloatIndirect = true;
            break;
          case ::LLVM::TDG::StreamInfo_Type_AT:
          case ::LLVM::TDG::StreamInfo_Type_ST:
            if (depS->getEnabledStoreFunc() &&
                !depS->isMergedLoadStoreDepStream()) {
              canFloatIndirect = true;
            }
            break;
          default:
            break;
          }
          if (canFloatIndirect && depS->addrBaseStreams.size() == 1) {
            // Only dependent on this direct stream.
            auto depConfig = depS->allocateCacheConfigureData(
                args.seqNum, true /* isIndirect */);
            streamConfigureData->addUsedBy(depConfig);
            // Remember the decision.
            auto &depDynS = depS->getDynamicStream(args.seqNum);
            depDynS.offloadedToCache = true;
            this->se->numFloated++;
            S_DPRINTF(depS, "Offload as indirect.\n");
            assert(offloadedStreamConfigMap.emplace(depS, depConfig).second &&
                   "Already offloaded this indirect stream.");
            // ! Pure hack here to indclude merged stream of this indirect
            // ! stream.
            for (auto mergedStreamId : depS->getMergedPredicatedStreams()) {
              auto mergedS = this->se->getStream(mergedStreamId.id().id());
              auto mergedConfig = mergedS->allocateCacheConfigureData(
                  args.seqNum, true /* isIndirect */);
              mergedConfig->isPredicated = true;
              mergedConfig->isPredicatedTrue = mergedStreamId.pred_true();
              mergedConfig->predicateStreamId = depDynS.dynamicStreamId;
              /**
               * Remember the decision.
               */
              mergedS->getDynamicStream(args.seqNum).offloadedToCache = true;
              this->se->numFloated++;
              assert(offloadedStreamConfigMap.emplace(mergedS, mergedConfig)
                         .second &&
                     "Merged stream already offloaded.");
              streamConfigureData->addUsedBy(mergedConfig);
            }
          }
        }
        // ! Disable one iteration behind indirect streams so far.
        // if (streamConfigureData->indirectStreams.empty()) {
        //   // Not found a valid indirect stream, let's try to search for
        //   // a indirect stream that is one iteration behind.
        //   for (auto backDependentStream : S->backDepStreams) {
        //     if (backDependentStream->getStreamType() != "phi") {
        //       continue;
        //     }
        //     if (backDependentStream->backBaseStreams.size() != 1) {
        //       continue;
        //     }
        //     for (auto indirectStream : backDependentStream->addrDepStreams)
        //     {
        //       if (indirectStream == S) {
        //         continue;
        //       }
        //       if (indirectStream->getStreamType() != "load") {
        //         continue;
        //       }
        //       if (indirectStream->addrBaseStreams.size() != 1) {
        //         continue;
        //       }
        //       // We found one valid indirect stream that is one iteration
        //       // behind S.
        //       streamConfigureData->addUsedBy(
        //           indirectStream->allocateCacheConfigureData(args.seqNum));
        //       streamConfigureData->indirectStreams.back()
        //           ->isOneIterationBehind = true;
        //       break;
        //     }
        //     if (!streamConfigureData->indirectStreams.empty()) {
        //       // We already found one.
        //       break;
        //     }
        //   }
        // }
      }

      /**
       * Merged predicated streams are always offloaded.
       */
      for (auto mergedStreamId : S->getMergedPredicatedStreams()) {
        auto mergedS = this->se->getStream(mergedStreamId.id().id());
        auto mergedConfig = mergedS->allocateCacheConfigureData(
            args.seqNum, true /* isIndirect */);
        mergedConfig->isPredicated = true;
        mergedConfig->isPredicatedTrue = mergedStreamId.pred_true();
        mergedConfig->predicateStreamId = dynStream.dynamicStreamId;
        /**
         * Remember the decision.
         */
        mergedS->getDynamicStream(args.seqNum).offloadedToCache = true;
        this->se->numFloated++;
        assert(offloadedStreamConfigMap.emplace(mergedS, mergedConfig).second &&
               "Merged stream already offloaded.");
        streamConfigureData->addUsedBy(mergedConfig);
      }
      /**
       * ValueDepStreams are always offloaded.
       */
      int numOffloadedValueDepStreams = 0;
      for (auto valueDepS : S->valueDepStreams) {
        auto valueDepConfig = valueDepS->allocateCacheConfigureData(
            args.seqNum, true /* isIndirect */);
        /**
         * Remember the decision.
         */
        valueDepS->getDynamicStream(args.seqNum).offloadedToCache = true;
        this->se->numFloated++;
        assert(offloadedStreamConfigMap.emplace(valueDepS, valueDepConfig)
                   .second &&
               "ValueDepStream already offloaded.");
        streamConfigureData->addUsedBy(valueDepConfig);
        numOffloadedValueDepStreams++;
      }

      cacheStreamConfigVec->push_back(streamConfigureData);
    }
  }

  this->floatReductionStreams(args, streamRegion, configStreams,
                              offloadedStreamConfigMap);

  // Sanity check for some offload decision.
  for (auto &S : configStreams) {
    auto &dynS = S->getDynamicStream(args.seqNum);
    if (!dynS.offloadedToCache) {
      if (S->getMergedPredicatedStreams().size() > 0) {
        S_PANIC(S, "Should offload streams with merged streams.");
      }
      if (S->isMergedPredicated()) {
        S_PANIC(S, "MergedStream not offloaded.");
      }
    }
  }

  // Send all the floating streams in one packet.
  if (!cacheStreamConfigVec->empty()) {
    // Dummy paddr to make ruby happy.
    Addr initPAddr = 0;
    auto pkt = GemForgePacketHandler::createStreamControlPacket(
        initPAddr, this->se->cpuDelegator->dataMasterId(), 0,
        MemCmd::Command::StreamConfigReq,
        reinterpret_cast<uint64_t>(cacheStreamConfigVec));
    for (const auto &config : *cacheStreamConfigVec) {
      SE_DPRINTF_(CoreRubyStreamLife, "%s: Send FloatConfig.\n",
                  config->dynamicId);
    }
    this->se->cpuDelegator->sendRequest(pkt);
  } else {
    delete cacheStreamConfigVec;
  }
}

void StreamFloatController::floatReductionStreams(
    const StreamConfigArgs &args, const ::LLVM::TDG::StreamRegion &streamRegion,
    std::list<Stream *> &configStreams, StreamCacheConfigMap &floatedMap) {
  for (auto &S : configStreams) {
    /**
     * We only float a ReductionStream if it only uses floated affine stream.
     */
    if (floatedMap.count(S) || !S->isReduction() ||
        !S->addrBaseStreams.empty()) {
      continue;
    }
    bool allBackBaseStreamsAreAffineAndFloated = true;
    std::vector<CacheStreamConfigureDataPtr> backBaseStreamConfigs;
    for (auto backBaseS : S->backBaseStreams) {
      if (backBaseS == S) {
        continue;
      }
      if (!backBaseS->isDirectMemStream() || !floatedMap.count(backBaseS)) {
        allBackBaseStreamsAreAffineAndFloated = false;
        break;
      }
      backBaseStreamConfigs.emplace_back(floatedMap.at(backBaseS));
    }
    if (!allBackBaseStreamsAreAffineAndFloated) {
      continue;
    }
    if (backBaseStreamConfigs.empty()) {
      S_PANIC(S, "ReductionStream without BackBaseStream.");
    }
    /**
     * Okay now we decided to float the ReductionStream. We randomly pick one
     * affine BackBaseStream A and make all other BackBaseStreams send to that
     * stream.
     */
    auto &dynStream = S->getDynamicStream(args.seqNum);
    auto reductionConfig = S->allocateCacheConfigureData(args.seqNum, true);
    // Reduction stream is always one iteration behind.
    reductionConfig->isOneIterationBehind = true;
    dynStream.offloadedToCache = true;
    this->se->numFloated++;
    floatedMap.emplace(S, reductionConfig);
    backBaseStreamConfigs.front()->addUsedBy(reductionConfig);
    for (int i = 1; i < backBaseStreamConfigs.size(); ++i) {
      auto &backBaseConfig = backBaseStreamConfigs.at(i);
      backBaseConfig->addSendTo(backBaseStreamConfigs.front());
      reductionConfig->addBaseOn(backBaseConfig);
    }
  }
}
