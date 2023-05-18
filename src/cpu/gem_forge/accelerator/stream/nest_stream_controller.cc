#include "stream_region_controller.hh"

#include "base/trace.hh"
#include "debug/StreamNest.hh"

#define DEBUG_TYPE StreamNest
#include "stream_log.hh"

#define SE_DPRINTF_RAW(se, X, format, args...)                                 \
  DPRINTF(X, "[SE%d]: " format, se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF_(X, format, args...)                                        \
  SE_DPRINTF_RAW(this->se, X, format, ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamNest, format, ##args)
#define WITH_SE_DPRINTF(se, format, args...)                                   \
  SE_DPRINTF_RAW(se, StreamNest, format, ##args)
#define SE_PANIC(format, args...)                                              \
  panic("[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)

namespace gem5 {

bool StreamRegionController::shouldRemoteConfigureNestRegion(
    StaticRegion &staticNestRegion) {
  if (!this->se->myParams->enableRemoteElimNestStreamConfig) {
    return false;
  }
  if (!staticNestRegion.allStreamsLoopEliminated) {
    return false;
  }
  for (auto S : staticNestRegion.streams) {
    /**
     * So far we allow InnerLoopBaseS to be remotely configured, but the
     * traffic to communicate back the value is not modelled.
     * TODO: Model the traffic to get value from RemoteConfig InnerLoopBaseS.
     */
    if (S->isInnerFinalValueUsedByCore()) {
      return false;
    }
    /**
     * Hash join can not remote configure. This can be done by checking that
     * NestOuterStream has no indirect stream. So far we hack this.
     * TODO: Disable remote config if OuterStream has IndirectS.
     */
    if (S->getStreamName().find("gfm.hash_join") != std::string::npos) {
      return false;
    }
  }
  return true;
}

void StreamRegionController::initializeNestStreams(
    const ::LLVM::TDG::StreamRegion &region, StaticRegion &staticRegion) {

  if (!region.is_nest()) {
    return;
  }

  const auto &nestConfigFuncInfo = region.nest_config_func();
  auto nestConfigFunc = std::make_shared<TheISA::ExecFunc>(
      se->getCPUDelegator()->getSingleThreadContext(), nestConfigFuncInfo);

  const auto &nestPredFuncInfo = region.nest_pred_func();
  ExecFuncPtr nestPredFunc = nullptr;
  bool nestPredRet = false;
  if (nestPredFuncInfo.name() != "") {
    nestPredFunc = std::make_shared<TheISA::ExecFunc>(
        se->getCPUDelegator()->getSingleThreadContext(), nestPredFuncInfo);
    nestPredRet = region.nest_pred_ret();
  }

  auto &staticNestConfig = staticRegion.nestConfig;
  staticNestConfig.configFunc = nestConfigFunc;
  staticNestConfig.predFunc = nestPredFunc;
  staticNestConfig.predRet = nestPredRet;

  bool hasBaseLoadS = false;
  for (const auto &arg : region.nest_config_func().args()) {
    if (arg.is_stream()) {
      // This is a stream input. Remember this in the base stream.
      staticNestConfig.baseStreamIds.insert(arg.stream_id());
      auto baseS = this->se->getStream(arg.stream_id());
      // It's possible that we don't have the outer S due to RemoteConfig.
      baseS->setDepNestRegion();
      if (baseS->isLoadStream()) {
        hasBaseLoadS = true;
      }
      const auto &outerRegion = this->getStaticRegion(baseS);
      staticNestConfig.outerRegion = outerRegion.region.region();
    }
  }

  assert(staticNestConfig.outerRegion != "");

  if (staticNestConfig.predFunc) {
    for (const auto &arg : region.nest_pred_func().args()) {
      if (arg.is_stream()) {
        // This is a stream input. Remember this in the base stream.
        staticNestConfig.baseStreamIds.insert(arg.stream_id());
        if (auto S = this->se->tryGetStream(arg.stream_id())) {
          // It's possible that we don't have the outer S due to RemoteConfig.
          S->setDepNestRegion();
          if (S->isLoadStream()) {
            hasBaseLoadS = true;
          }
        }
      }
    }
  }

  if (!hasBaseLoadS && this->shouldRemoteConfigureNestRegion(staticRegion)) {
    /**
     * We need a load stream for remote config.
     * So far I just try to find one from the outer loop.
     */
    assert(!staticNestConfig.baseStreamIds.empty());
    const auto &baseId = *staticNestConfig.baseStreamIds.begin();
    auto baseS = this->se->getStream(baseId);
    const auto &outerRegion = this->getStaticRegion(baseS);
    for (auto outerS : outerRegion.streams) {
      if (outerS->isLoadStream()) {
        S_DPRINTF(outerS,
                  "[Nest] ! Add Fake NestRegionDep to Enable Remote Config.\n");
        outerS->setDepNestRegion();
        staticNestConfig.baseStreamIds.insert(outerS->staticId);
        break;
      }
    }
  }

  SE_DPRINTF("[Nest] Init StaticNestConfig for region %s.\n", region.region());
}

void StreamRegionController::dispatchStreamConfigForNestStreams(
    const ConfigArgs &args, DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  for (const auto &nestRelativePath :
       staticRegion.region.nest_region_relative_paths()) {
    const auto &nestRegion = this->se->getStreamRegion(nestRelativePath);
    assert(nestRegion.is_nest());

    auto &staticNestRegion = this->getStaticRegion(nestRegion.region());

    // Initialize a DynNestConfig.
    auto &staticNestConfig = staticNestRegion.nestConfig;
    assert(staticNestConfig.outerRegion == staticRegion.region.region());

    dynRegion.nestConfigs.emplace_back(&staticNestRegion);
    auto &dynNestConfig = dynRegion.nestConfigs.back();
    dynNestConfig.configFunc = staticNestConfig.configFunc;
    dynNestConfig.predFunc = staticNestConfig.predFunc;

    dynNestConfig.isRemoteConfig =
        this->shouldRemoteConfigureNestRegion(staticNestRegion);
    if (dynNestConfig.isRemoteConfig) {
      SE_DPRINTF("[Nest] Remote NestConfig for region %s.\n",
                 staticNestRegion.region.region());
    }

    for (auto baseStreamId : staticNestConfig.baseStreamIds) {
      auto S = this->se->getStream(baseStreamId);
      S_DPRINTF(S, "[Nest] Add as BaseS for NestRegion %s.\n",
                staticNestRegion.region.region());
      dynNestConfig.baseStreams.insert(S);
      S->setDepNestRegion();
      auto &dynS = S->getDynStream(args.seqNum);
      dynS.setDepRemoteNestRegion(dynNestConfig.isRemoteConfig);
    }
  }
}

void StreamRegionController::executeStreamConfigForNestStreams(
    const ConfigArgs &args, DynRegion &dynRegion) {

  if (dynRegion.nestConfigs.empty()) {
    return;
  }

  assert(args.inputMap && "Missing InputMap.");
  assert(args.inputMap->count(::LLVM::TDG::ReservedStreamRegionId::
                                  NestConfigureFuncInputRegionId) &&
         "Missing InputVec for NestConfig.");
  auto &inputVec = args.inputMap->at(
      ::LLVM::TDG::ReservedStreamRegionId::NestConfigureFuncInputRegionId);

  int inputIdx = 0;

  for (auto &dynNestConfig : dynRegion.nestConfigs) {

    // Construct the NestConfigFunc formal params.
    {
      auto &formalParams = dynNestConfig.formalParams;
      const auto &configFuncInfo = dynNestConfig.configFunc->getFuncInfo();
      this->buildFormalParams(inputVec, inputIdx, configFuncInfo, formalParams);
    }

    // Construct the NestPredFunc formal params.
    if (dynNestConfig.predFunc) {
      auto &formalParams = dynNestConfig.predFormalParams;
      const auto &predFuncInfo = dynNestConfig.predFunc->getFuncInfo();
      this->buildFormalParams(inputVec, inputIdx, predFuncInfo, formalParams);
    }
  }

  SE_DPRINTF("[Nest] Execute Config: %s.\n",
             dynRegion.staticRegion->region.region());
}

StreamRegionController::DynRegion::DynNestConfig *
StreamRegionController::getFirstRemainingNestRegion(DynRegion &dynRegion) {

  if (dynRegion.nestConfigs.empty()) {
    return nullptr;
  }

  for (auto &dynNestConfig : dynRegion.nestConfigs) {
    if (dynNestConfig.lastConfigSeqNum ==
        DynRegion::DynNestConfig::InvalidConfigSeqNum) {
      continue;
    }
    if (dynNestConfig.nestDynRegions.empty()) {
      continue;
    }
    auto &staticNestRegion = *(dynNestConfig.staticRegion);
    SE_DPRINTF("[Nest] Outer %s. Nested %s ConfigSN %llu remains.\n",
               dynRegion.staticRegion->region.region(),
               staticNestRegion.region.region(),
               dynNestConfig.nestDynRegions.front().configSeqNum);
    return &dynNestConfig;
  }
  return nullptr;
}

void StreamRegionController::configureNestStream(
    DynRegion &dynRegion, DynRegion::DynNestConfig &dynNestConfig) {

  if (dynNestConfig.skipConfig) {
    return;
  }

  auto &staticRegion = *dynRegion.staticRegion;
  auto &staticNestRegion = *(dynNestConfig.staticRegion);
  auto &staticNestConfig = staticNestRegion.nestConfig;

  /**
   * It does not really make sense to configure future nested streams
   * if the nested loop is not eliminated. Here we check that the
   * current NestDynStream has trip count and is close to end,
   * before trying to configure next dynamic stream.
   */
  if (!staticNestRegion.region.loop_eliminated() &&
      staticNestRegion.dynRegions.size() > 0) {
    const auto &lastDynNestRegion = staticNestRegion.dynRegions.back();
    auto firstNestStream = staticNestRegion.streams.front();
    const auto &lastDynNestStream =
        firstNestStream->getDynStream(lastDynNestRegion.seqNum);
    if (lastDynNestStream.endDispatched ||
        (lastDynNestStream.hasTotalTripCount() &&
         lastDynNestStream.FIFOIdx.entryIdx + 2 >=
             lastDynNestStream.getTotalTripCount())) {
      // continue.
    } else {
      DYN_S_DPRINTF(lastDynNestStream.dynStreamId,
                    "[Nest] NestedLoop not Eliminated. TripCount %ld "
                    "NextElemIdx %lu EndDispatched %d NumDynRegions %d.\n",
                    lastDynNestStream.getTotalTripCount(),
                    lastDynNestStream.FIFOIdx.entryIdx,
                    lastDynNestStream.endDispatched,
                    staticNestRegion.dynRegions.size());
      return;
    }
  }

  // Wait for the LoopBound.
  auto nextElemIdx = dynNestConfig.nextConfigElemIdx;
  const auto &dynBound = dynRegion.loopBound;
  if (staticRegion.region.is_loop_bound()) {
    if (nextElemIdx > dynBound.nextElemIdx) {
      SE_DPRINTF("[Nest] Wait For LoopBound: %llu >= %llu %s.\n", nextElemIdx,
                 dynBound.nextElemIdx, staticNestRegion.region.region());
      return;
    }
  }

  // Check the base elements are value ready.
  std::unordered_set<StreamElement *> baseElems;
  for (auto baseS : dynNestConfig.baseStreams) {
    auto &baseDynS = baseS->getDynStream(dynRegion.seqNum);
    if (baseDynS.hasTotalTripCount() &&
        nextElemIdx >= baseDynS.getTotalTripCount()) {
      DYN_S_DPRINTF(baseDynS.dynStreamId,
                    "[Nest] Overflow Next %lu TripCount %lu.\n", nextElemIdx,
                    baseDynS.getTotalTripCount());
      dynNestConfig.skipConfig = true;
      return;
    }
    auto baseE = baseDynS.getElemByIdx(nextElemIdx);
    if (!baseE) {
      if (baseDynS.FIFOIdx.entryIdx > nextElemIdx) {
        DYN_S_DPRINTF(baseDynS.dynStreamId,
                      "Failed to get elem %llu for NestConfig. The "
                      "TotalTripCount must be 0. Skip.\n",
                      dynNestConfig.nextConfigElemIdx);
        dynNestConfig.nextConfigElemIdx++;
        return;
      } else {
        // The base element is not allocated yet.
        S_DPRINTF(baseS,
                  "[Nest] BaseElem %llu not allocated yet for NestConfig. "
                  "Current NestRegions %d.\n",
                  nextElemIdx, staticNestRegion.dynRegions.size());
        dynNestConfig.skipConfig = true;
        auto allocElemCallback = [&dynNestConfig](DynStream *dynS,
                                                  uint64_t elemIdx) -> bool {
          if (elemIdx >= dynNestConfig.nextConfigElemIdx) {
            // We are done.
            dynNestConfig.skipConfig = false;
            return true;
          } else {
            // We are not done.
            return false;
          }
        };
        baseDynS.registerAllocElemCallback(allocElemCallback);
        return;
      }
    }
    if (!baseE->isValueReady) {
      S_ELEMENT_DPRINTF(baseE, "[Nest] Value not ready. Skip.\n");
      dynNestConfig.skipConfig = true;
      auto elemValueReadyCallback =
          [&dynNestConfig](StreamElement *elem) -> bool {
        dynNestConfig.skipConfig = false;
        return true;
      };
      baseE->registerValueReadyCallback(elemValueReadyCallback);
      return;
    }
    if (baseE->isAnyValueFaulted()) {
      S_ELEMENT_DPRINTF(baseE, "[Nest] Value Faulted.\n");
      return;
    }
    baseElems.insert(baseE);
  }
  for (auto baseE : baseElems) {
    if (baseE->isLastElement()) {
      S_ELEMENT_DPRINTF(baseE, "[Nest] Reached TripCount.\n");
      return;
    }
  }

  /**
   * To avoid deadlock when there is InnerLoopBaseE, make sure that the
   * InnerLoopDepS has allocated that element.
   */
  for (auto S : staticRegion.streams) {
    if (S->innerLoopBaseEdges.empty()) {
      continue;
    }
    auto &dynS = S->getDynStream(dynRegion.seqNum);
    if (dynS.FIFOIdx.entryIdx <= nextElemIdx + 1) {
      // The consuming InnerLoopDepElem is not allocated.
      SE_DPRINTF("[Nest] InnerLoopDepE Not Alloc %s <= %lu + 1.\n",
                 dynS.FIFOIdx.entryIdx, nextElemIdx + 1);

      /**
       * There is a potential deadlock here: when all InnerLoopDepS
       * have one element allocated and reaches max alloc size.
       * We don't fix that for now, but add a sanity check here.
       */
      if (S->getAllocSize() == S->maxSize &&
          S->maxSize <= S->getNumDynStreams()) {
        bool allAllocLessEqualOne = true;
        for (const auto &dynS : S->dynamicStreams) {
          if (dynS.allocSize > 1) {
            allAllocLessEqualOne = false;
            break;
          }
        }
        if (allAllocLessEqualOne) {
          NO_LOC_INFORM("!!! !!! Potential Deadlock for InnerLoopDepS.\n");
          S->dump();
          DYN_S_PANIC(dynS.dynStreamId,
                      "Potential Deadlock for InnerLoopDepS.");
        }
      }

      return;
    }
  }

  /**
   * If this is RemoteConfig, try to configure at the specified SE.
   */
  auto nestSE = this->se;
  if (dynNestConfig.isRemoteConfig) {
    int remoteBank = -1;
    for (auto baseE : baseElems) {
      if (baseE->isFloatElem() && baseE->stream->isLoadStream()) {
        if (baseE->hasRemoteBank()) {
          auto elemRemoteBank = baseE->getRemoteBank();
          S_ELEMENT_DPRINTF(baseE, "[Nest] RemoteBank %d.\n", elemRemoteBank);
          if (remoteBank == -1) {
            remoteBank = elemRemoteBank;
          } else if (remoteBank != elemRemoteBank) {
            S_ELEMENT_PANIC(baseE, "[Nest] Mismatch in RemoteBank.");
          }
        } else {
          S_ELEMENT_DPRINTF(baseE, "[Nest] Miss RemoteBank.\n");
          return;
        }
      } else {
        S_ELEMENT_DPRINTF(baseE,
                          "[Nest] NoRemoteBank IsFloatElem %d IsLoadS %d.\n",
                          baseE->isFloatElem(), baseE->stream->isLoadStream());
      }
    }
    if (remoteBank == -1) {
      SE_PANIC("No RemoteBank for %s.", staticNestRegion.region.region());
    }
    assert(remoteBank >= 0);
    assert(remoteBank < StreamEngine::GlobalStreamEngines.size() &&
           "[Nest] Overflow RemoteBank.");
    // auto selected = rand() % numSEs;
    // auto selected = 1;
    auto selected = remoteBank;
    nestSE = StreamEngine::GlobalStreamEngines.at(selected);
    SE_DPRINTF("[Nest] RemoteConfig at CPU %d.\n",
               nestSE->cpuDelegator->cpuId());
  }

  /**
   * We also limit the number of dynamic nest regions at the same time.
   * Previously this is controlled by limit the number of elements allocated for
   * outer loop streams, however, that limits our prefetch benefits (see
   * StreamThrottler). Hence now we isolate these two parameters.
   */
  if (staticNestRegion.region.loop_eliminated() &&
      staticNestRegion.dynRegions.size() >=
          this->se->myParams->elimNestStreamInstances) {
    SE_DPRINTF("[Nest] Reach MaxNestRegions %d > %d.\n",
               staticNestRegion.dynRegions.size(),
               this->se->myParams->elimNestStreamInstances);
    for (auto nestS : staticNestRegion.streams) {
      nestS->statistic.remoteNestConfigMaxRegions++;
    }
    return;
  }
  /**
   * Limit by the total number of ElimStreamInstances at the NestSE.
   */
  auto numDynNestRegions = nestSE->regionController->getNumDynRegion(
      staticNestRegion.region.region());
  if (numDynNestRegions >= this->se->myParams->elimNestStreamInstances) {
    SE_DPRINTF("[Nest] Too Many Infly RemoteNestConfig %d. Skip.\n",
               numDynNestRegions);
    dynNestConfig.skipConfig = true;
    auto dynRegionReleaseCallback =
        [this, &dynNestConfig](StaticRegion *staticRegion) -> bool {
      if (staticRegion->dynRegions.size() <
          this->se->myParams->elimNestStreamInstances) {
        // We are done.
        dynNestConfig.skipConfig = false;
        return true;
      } else {
        // We are not done yet.
        return false;
      }
    };
    nestSE->regionController->getStaticRegion(staticNestRegion.region.region())
        .registerDynRegionReleaseCallback(dynRegionReleaseCallback);
    return;
  }

  /**
   * Since allocating a new stream will take one element, we check that
   * there are available free elements.
   */
  if (nestSE->numFreeFIFOEntries < staticNestRegion.streams.size()) {
    SE_DPRINTF("[Nest] No Total Free Elem to allocate NestConfig, Has %d, "
               "Required %d.\n",
               nestSE->numFreeFIFOEntries, staticNestRegion.streams.size());
    return;
  }
  for (auto S : staticNestRegion.streams) {
    if (S->getAllocSize() + 1 >= S->maxSize) {
      S_DPRINTF(S,
                "[Nest] No Free Elem to allocate NestConfig, AllocSize %d, "
                "MaxSize %d. Skip.\n",
                S->getAllocSize(), S->maxSize);
      dynNestConfig.skipConfig = true;
      auto freeElemCallback = [&dynNestConfig](Stream *S) -> bool {
        if (S->getAllocSize() + 1 >= S->maxSize) {
          // Still not available.
          return false;
        } else {
          dynNestConfig.skipConfig = false;
          return true;
        }
      };
      S->registerFreeElemCallback(freeElemCallback);
      return;
    }
  }

  // All base elements are value ready.
  auto getStreamValue = GetStreamValueFromElementSet(baseElems, "[Nest]");

  /**
   * If we have predication, evaluate the predication function first.
   */
  if (dynNestConfig.predFunc) {
    auto predActualParams = convertFormalParamToParam(
        dynNestConfig.predFormalParams, getStreamValue);
    auto predRet = dynNestConfig.predFunc->invoke(predActualParams).front();
    if (predRet != staticNestConfig.predRet) {
      SE_DPRINTF("[Nest] Predicated Skip (%d != %d) NestRegion %s.\n", predRet,
                 staticNestConfig.predRet, staticNestRegion.region.region());
      dynNestConfig.nextConfigElemIdx++;
      return;
    }
  }

  auto actualParams =
      convertFormalParamToParam(dynNestConfig.formalParams, getStreamValue);

  if (Debug::StreamNest) {
    SE_DPRINTF("[Nest] Value ready. Configure NestRegion %s, "
               "OuterElementIdx "
               "%lu, ActualParams:\n",
               staticNestRegion.region.region(),
               dynNestConfig.nextConfigElemIdx);
    for (const auto &actualParam : actualParams) {
      SE_DPRINTF("[Nest]   Param %s.\n", actualParam.print());
    }
  }

  auto &isaHandler = nestSE->regionController->isaHandler;
  auto &isaSE = isaHandler.getISAStreamEngine();
  isaSE.reset();
  isaSE.setOuterSE(this->se, dynRegion.seqNum);
  // Initialize all outer loop.
  std::vector<StaticRegion *> outerRegions;
  auto curRegion = dynRegion.staticRegion;
  while (true) {
    outerRegions.push_back(curRegion);
    if (!curRegion->nestConfig.configFunc) {
      break;
    }
    assert(curRegion->nestConfig.outerRegion != "");
    curRegion = &this->getStaticRegion(curRegion->nestConfig.outerRegion);
  }
  for (auto i = 0; i < outerRegions.size(); ++i) {
    nestSE->tryInitializeStreams(
        outerRegions.at(outerRegions.size() - i - 1)->region);
  }
  auto configFuncStartSeqNum = dynNestConfig.getConfigSeqNum(
      nestSE, dynNestConfig.nextConfigElemIdx, dynRegion.seqNum);
  dynNestConfig.configFunc->invoke(actualParams, &isaHandler,
                                   configFuncStartSeqNum);

  // Remember the NestConfigSeqNum and parent/child relationship.
  InstSeqNum nestConfigSeqNum = configFuncStartSeqNum;
  {
    auto S = staticNestRegion.streams.front();
    auto &dynS = nestSE->getStream(S->staticId)->getLastDynStream();
    // auto &dynS = S->getLastDynStream();
    auto dynSTripCount = dynS.getTotalTripCount();
    if (dynSTripCount == 0) {
      DYN_S_PANIC(dynS.dynStreamId, "NestStream has TripCount %d.",
                  dynSTripCount);
    }
    nestConfigSeqNum = dynS.configSeqNum;
  }
  dynNestConfig.lastConfigSeqNum = nestConfigSeqNum;
  dynNestConfig.nestDynRegions.emplace_back(nestConfigSeqNum, nestSE);
  auto &dynNestRegion =
      nestSE->regionController->getDynRegion("NestConfig", nestConfigSeqNum);
  dynNestRegion.nestParentSE = this->se;
  dynNestRegion.nestParentDynConfig = &dynNestConfig;

  SE_DPRINTF("[Nest] Value ready. Config NestRegion %s OuterConfigSeqNum %lu "
             "OuterElemIdx %lu ConfigFuncStartSeqNum %lu ConfigSeqNum %lu "
             "Configured:\n",
             staticNestRegion.region.region(), dynRegion.seqNum,
             dynNestConfig.nextConfigElemIdx, configFuncStartSeqNum,
             nestConfigSeqNum);
  if (Debug::StreamNest) {
    for (auto S : staticNestRegion.streams) {
      auto &dynS = nestSE->getStream(S->staticId)->getLastDynStream();
      SE_DPRINTF("[Nest] TripCount %8lld  %s.\n", dynS.getTotalTripCount(),
                 dynS.dynStreamId);
    }
  }

  if (nestSE != this->se) {
    // This is a RemoteNestConfigure.
    for (auto S : staticNestRegion.streams) {
      nestSE->getStream(S->staticId)
          ->statistic.sampleRemoteNestConfig(this->se->cpuDelegator->cpuId(),
                                             nestSE->cpuDelegator->cpuId());
    }
  }

  dynNestConfig.nextConfigElemIdx++;
}

InstSeqNum
    StreamRegionController::DynRegion::DynNestConfig::GlobalNestConfigSeqNum =
        StreamRegionController::DynRegion::DynNestConfig::
            GlobalNestConfigSeqNumStart;

InstSeqNum StreamRegionController::DynRegion::DynNestConfig::getConfigSeqNum(
    StreamEngine *se, uint64_t elemIdx, uint64_t outSeqNum) const {
  /**
   * New GlobalNestConfigSeqNum implementation.
   * To support recursive nesting, the original implementation that takes
   * an offset from the outer region's ConfigSeqNum can not guarantee that
   * every DynRegion has a unique ConfigSeqNum.
   *
   * To fix this, I break the order between core configured regions and
   * se configured regions (i.e. nest regions), and maintain a global
   * monotonically increasing ConfigSeqNum for all nest regions.
   *
   * This global SeqNum starts from a large number and hopefully that avoids
   * collision with core configured regions.
   *
   * TODO: Perhaps I should rename it to something other than SeqNum.
   *
   * This needs to be smaller than the StreamEnd SeqNum from core.
   * We use outSeqNum + 1 + elementIdx * (instOffset + 1).
   * Notice that we will subtract the Configuration function instructions
   * so that there is no gap to the final executed StreamConfig.
   */
  const int numConfigInsts = this->configFunc->getNumInstsBeforeStreamConfig();
  const int instOffset = 1;
  // InstSeqNum ret = outSeqNum + 1 + elemIdx * (instOffset + 1);
  InstSeqNum ret = GlobalNestConfigSeqNum + 1;

  GlobalNestConfigSeqNum += instOffset + 1;

  assert(ret > numConfigInsts && "Cann't subtract NumConfigInsts.");
  ret -= (numConfigInsts);

  WITH_SE_DPRINTF(se,
                  "[Nest] ConfigSeqNum OutSN %lu NumInst %lu ElemIdx %lu.\n",
                  outSeqNum, numConfigInsts, elemIdx);
  assert(ret != InvalidConfigSeqNum && "Generated InvalidConfigSeqNum.");
  return ret;
}
} // namespace gem5
