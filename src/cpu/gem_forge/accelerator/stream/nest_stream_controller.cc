#include "stream_region_controller.hh"

#include "base/trace.hh"
#include "debug/StreamNest.hh"

#define DEBUG_TYPE StreamNest
#include "stream_log.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamNest, format, ##args)
#define SE_PANIC(format, args...)                                              \
  panic("[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)

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

  for (const auto &arg : region.nest_config_func().args()) {
    if (arg.is_stream()) {
      // This is a stream input. Remember this in the base stream.
      auto S = this->se->getStream(arg.stream_id());
      staticNestConfig.baseStreams.insert(S);
      S->setDepNestRegion();
    }
  }

  if (nestPredFunc) {
    for (const auto &arg : region.nest_pred_func().args()) {
      if (arg.is_stream()) {
        // This is a stream input. Remember this in the base stream.
        auto S = this->se->getStream(arg.stream_id());
        staticNestConfig.baseStreams.insert(S);
        S->setDepNestRegion();
      }
    }
  }

  SE_DPRINTF("[Nest] Initialized StaticNestConfig for region %s.\n",
             region.region());
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

    dynRegion.nestConfigs.emplace_back(&staticNestRegion);
    auto &dynNestConfig = dynRegion.nestConfigs.back();
    dynNestConfig.configFunc = staticNestConfig.configFunc;
    dynNestConfig.predFunc = staticNestConfig.predFunc;
  }
}

void StreamRegionController::executeStreamConfigForNestStreams(
    const ConfigArgs &args, DynRegion &dynRegion) {

  if (dynRegion.nestConfigs.empty()) {
    return;
  }

  assert(dynRegion.nestConfigs.size() == 1 &&
         "Multiple Nesting is not supported.");

  auto &dynNestConfig = dynRegion.nestConfigs.front();

  assert(args.inputMap && "Missing InputMap.");
  assert(args.inputMap->count(::LLVM::TDG::ReservedStreamRegionId::
                                  NestConfigureFuncInputRegionId) &&
         "Missing InputVec for NestConfig.");
  auto &inputVec = args.inputMap->at(
      ::LLVM::TDG::ReservedStreamRegionId::NestConfigureFuncInputRegionId);

  int inputIdx = 0;

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

  SE_DPRINTF("[Nest] Execute Config: %s.\n",
             dynRegion.staticRegion->region.region());
}

bool StreamRegionController::checkRemainingNestRegions(
    const DynRegion &dynRegion) {

  if (dynRegion.nestConfigs.empty()) {
    return false;
  }

  for (const auto &dynNestConfig : dynRegion.nestConfigs) {
    if (dynNestConfig.configSeqNums.empty()) {
      continue;
    }
    auto &staticNestRegion = *(dynNestConfig.staticRegion);
    if (staticNestRegion.dynRegions.empty()) {
      continue;
    }
    auto lastNestConfigSeqNum = dynNestConfig.configSeqNums.back();
    const auto &firstNestDynRegion = staticNestRegion.dynRegions.front();
    if (firstNestDynRegion.seqNum <= lastNestConfigSeqNum) {
      SE_PANIC("[Nest] Ended/Rewinded outer %s. Nested %s ConfigSeqNum %lu "
               "still exists.",
               dynRegion.staticRegion->region.region(),
               staticNestRegion.region.region(), firstNestDynRegion.seqNum);
    }
  }
  return false;
}

void StreamRegionController::configureNestStream(
    DynRegion &dynRegion, DynRegion::DynNestConfig &dynNestConfig) {

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
                    "[Nest] NestedLoop not Eliminated. TotalTripCount %ld "
                    "NextElementIdx %lu EndDispatched %d NumDynRegions %d.\n",
                    lastDynNestStream.getTotalTripCount(),
                    lastDynNestStream.FIFOIdx.entryIdx,
                    lastDynNestStream.endDispatched,
                    staticNestRegion.dynRegions.size());
      return;
    }
  }

  /**
   * Since allocating a new stream will take one element, we check that
   * there are available free elements.
   */
  if (this->se->numFreeFIFOEntries < staticNestRegion.streams.size()) {
    SE_DPRINTF("[Nest] No Total Free Element to allocate NestConfig, Has %d, "
               "Required %d.\n",
               this->se->numFreeFIFOEntries, staticNestRegion.streams.size());
    return;
  }
  for (auto S : staticNestRegion.streams) {
    if (S->getAllocSize() + 1 >= S->maxSize) {
      S_DPRINTF(S,
                "[Nest] No Free Element to allocate NestConfig, AllocSize %d, "
                "MaxSize %d.\n",
                S->getAllocSize(), S->maxSize);
      return;
    }
  }

  auto nextElementIdx = dynNestConfig.nextElemIdx;
  std::unordered_set<StreamElement *> baseElements;
  for (auto baseS : staticNestConfig.baseStreams) {
    auto &baseDynS = baseS->getDynStream(dynRegion.seqNum);
    auto baseElement = baseDynS.getElementByIdx(nextElementIdx);
    if (!baseElement) {
      if (baseDynS.FIFOIdx.entryIdx > nextElementIdx) {
        DYN_S_DPRINTF(baseDynS.dynStreamId,
                      "Failed to get element %llu for NestConfig. The "
                      "TotalTripCount must be 0. Skip.\n",
                      dynNestConfig.nextElemIdx);
        dynNestConfig.nextElemIdx++;
        return;
      } else {
        // The base element is not allocated yet.
        S_DPRINTF(baseS,
                  "[Nest] BaseElement %llu not allocated yet for NestConfig. "
                  "Current NestRegions %d.\n",
                  nextElementIdx, staticNestRegion.dynRegions.size());
        return;
      }
    }
    if (!baseElement->isValueReady) {
      // S_ELEMENT_DPRINTF(baseElement,
      //                   "[Nest] Value not ready for NestConfig.\n");
      return;
    }
    baseElements.insert(baseElement);
  }
  for (auto baseE : baseElements) {
    if (baseE->isLastElement()) {
      S_ELEMENT_DPRINTF(baseE, "[Nest] Reached TripCount.\n");
      return;
    }
  }

  // All base elements are value ready.
  auto getStreamValue = GetStreamValueFromElementSet(baseElements, "[Nest]");

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
      dynNestConfig.nextElemIdx++;
      return;
    }
  }

  auto actualParams =
      convertFormalParamToParam(dynNestConfig.formalParams, getStreamValue);

  if (Debug::StreamNest) {
    SE_DPRINTF("[Nest] Value ready. Configure NestRegion %s, OuterElementIdx "
               "%lu, ActualParams:\n",
               staticNestRegion.region.region(), dynNestConfig.nextElemIdx);
    for (const auto &actualParam : actualParams) {
      SE_DPRINTF("[Nest]   Param %s.\n", actualParam.print());
    }
  }

  this->isaHandler.resetISAStreamEngine();
  auto configFuncStartSeqNum = dynNestConfig.getConfigSeqNum(
      dynNestConfig.nextElemIdx, dynRegion.seqNum);
  dynNestConfig.configFunc->invoke(actualParams, &this->isaHandler,
                                   configFuncStartSeqNum);

  // Sanity check that nest streams have same TotalTripCount.
  bool isFirstDynS = true;
  auto totalTripCount = DynStream::InvalidTotalTripCount;
  InstSeqNum nestConfigSeqNum = configFuncStartSeqNum;
  for (auto S : staticNestRegion.streams) {
    auto &dynS = S->getLastDynStream();
    auto dynSTotalTripCount = dynS.getTotalTripCount();
    if (dynSTotalTripCount == 0) {
      DYN_S_PANIC(dynS.dynStreamId, "NestStream has TotalTripCount %d.",
                  totalTripCount);
    }
    if (isFirstDynS) {
      totalTripCount = dynSTotalTripCount;
      nestConfigSeqNum = dynS.configSeqNum;
      isFirstDynS = false;
    } else {
      if (totalTripCount != dynSTotalTripCount) {
        DYN_S_PANIC(dynS.dynStreamId,
                    "NestStream has TotalTripCount %d, while others have %d.",
                    dynSTotalTripCount, totalTripCount);
      }
    }
  }
  // Remember the config seq num.
  dynNestConfig.configSeqNums.push_back(nestConfigSeqNum);

  SE_DPRINTF("[Nest] Value ready. Config NestRegion %s OuterElemIdx %lu "
             "ConfigFuncStartSeqNum %lu ConfigSeqNum %lu TripCount %d "
             "Configured:\n",
             staticNestRegion.region.region(), dynNestConfig.nextElemIdx,
             configFuncStartSeqNum, nestConfigSeqNum, totalTripCount);
  if (Debug::StreamNest) {
    for (auto S : staticNestRegion.streams) {
      auto &dynS = S->getLastDynStream();
      SE_DPRINTF("[Nest]   %s.\n", dynS.dynStreamId);
    }
  }

  dynNestConfig.nextElemIdx++;
}

InstSeqNum StreamRegionController::DynRegion::DynNestConfig::getConfigSeqNum(
    uint64_t elementIdx, uint64_t outSeqNum) const {
  /**
   * This needs to be smaller than the StreamEnd SeqNum from core.
   * We use outSeqNum + 1 + elementIdx * (instOffset + 1).
   * Notice that we will subtract the Configuration function instructions
   * so that there is no gap to the final executed StreamConfig.
   */
  const int numConfigInsts = this->configFunc->getNumInstructions();
  const int instOffset = 1;
  InstSeqNum ret = outSeqNum + 1 + elementIdx * (instOffset + 1);

  assert(ret > numConfigInsts && "Cann't subtract NumConfigInsts.");
  ret -= (numConfigInsts - 1);

  return ret;
}
