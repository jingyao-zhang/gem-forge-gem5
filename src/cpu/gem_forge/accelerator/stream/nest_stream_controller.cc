#include "nest_stream_controller.hh"

#include "base/trace.hh"
#include "debug/StreamNest.hh"

#define DEBUG_TYPE StreamNest
#include "stream_log.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamNest, format, ##args)

NestStreamController::NestStreamController(StreamEngine *_se)
    : se(_se), isaHandler(_se->getCPUDelegator()) {}

NestStreamController::~NestStreamController() {}

void NestStreamController::initializeNestConfig(
    const ::LLVM::TDG::StreamRegion &region) {
  assert(region.is_nest() && "This is not a nest region.");

  const auto &nestConfigFuncInfo = region.nest_config_func();
  auto nestConfigFunc = std::make_shared<TheISA::ExecFunc>(
      se->getCPUDelegator()->getSingleThreadContext(), nestConfigFuncInfo);

  this->staticNestConfigMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(region.region()),
      std::forward_as_tuple(region, nestConfigFunc));

  auto &staticNestConfig = this->staticNestConfigMap.at(region.region());
  for (const auto &arg : region.nest_config_func().args()) {
    if (arg.is_stream()) {
      // This is a stream input. Remember this in the base stream.
      auto S = this->se->getStream(arg.stream_id());
      staticNestConfig.baseStreams.insert(S);
      S->setDepNestRegion();
    }
  }
  for (const auto &streamInfo : region.streams()) {
    auto S = this->se->getStream(streamInfo.id());
    staticNestConfig.configStreams.insert(S);
  }

  SE_DPRINTF("[Nest] Initialized StaticNestConfig for region %s.\n",
             region.region());
}

void NestStreamController::dispatchStreamConfig(const ConfigArgs &args) {
  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->se->getStreamRegion(infoRelativePath);
  for (const auto &nestRelativePath :
       streamRegion.nest_region_relative_paths()) {
    const auto &nestRegion = this->se->getStreamRegion(nestRelativePath);
    if (this->staticNestConfigMap.count(nestRegion.region())) {
      // Initialize a DynNestConfig.
      auto &staticNestConfig =
          this->staticNestConfigMap.at(nestRegion.region());
      staticNestConfig.dynConfigs.emplace_back(&staticNestConfig, args.seqNum,
                                               staticNestConfig.configFunc);
      assert(this->activeDynNestConfigMap
                 .emplace(
                     std::piecewise_construct,
                     std::forward_as_tuple(args.seqNum),
                     std::forward_as_tuple(&staticNestConfig.dynConfigs.back()))
                 .second &&
             "Multiple nesting is not supported.");
      SE_DPRINTF("[Nest] Initialized DynNestConfig for region %s.\n",
                 nestRegion.region());
    }
  }
}

void NestStreamController::executeStreamConfig(const ConfigArgs &args) {
  if (this->activeDynNestConfigMap.count(args.seqNum)) {
    auto &dynNestConfig = *this->activeDynNestConfigMap.at(args.seqNum);

    const auto &info = dynNestConfig.configFunc->getFuncInfo();
    auto &formalParams = dynNestConfig.formalParams;
    assert(args.inputMap && "Missing InputMap.");
    assert(args.inputMap->count(DynamicStreamId::InvalidStaticStreamId) &&
           "Missing InputVec for NestConfig.");
    auto &inputVec = args.inputMap->at(DynamicStreamId::InvalidStaticStreamId);

    // Construct the NestConfigFunc formal params.
    int inputIdx = 0;
    for (const auto &arg : info.args()) {
      if (arg.is_stream()) {
        // This is a stream input.
        formalParams.emplace_back();
        auto &formalParam = formalParams.back();
        formalParam.isInvariant = false;
        formalParam.baseStreamId = arg.stream_id();
      } else {
        if (inputIdx >= inputVec.size()) {
          panic("Missing input for %s: Given %llu, inputIdx %d.", info.name(),
                inputVec.size(), inputIdx);
        }
        formalParams.emplace_back();
        auto &formalParam = formalParams.back();
        formalParam.isInvariant = true;
        formalParam.invariant = inputVec.at(inputIdx);
        inputIdx++;
      }
    }

    dynNestConfig.configExecuted = true;

    SE_DPRINTF("[Nest] Executed DynNestConfig for region %s.\n",
               dynNestConfig.staticNestConfig->region.region());
  }
}

void NestStreamController::rewindStreamConfig(const ConfigArgs &args) {
  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->se->getStreamRegion(infoRelativePath);
  for (const auto &nestRelativePath :
       streamRegion.nest_region_relative_paths()) {
    const auto &nestRegion = this->se->getStreamRegion(nestRelativePath);
    if (this->staticNestConfigMap.count(nestRegion.region())) {
      // Release the DynNestConfig.
      auto &staticNestConfig =
          this->staticNestConfigMap.at(nestRegion.region());
      assert(!staticNestConfig.dynConfigs.empty() && "Missing DynNestConfig.");
      const auto &dynNestConfig = staticNestConfig.dynConfigs.back();
      assert(dynNestConfig.seqNum == args.seqNum &&
             "Mismatch in rewind seqNum.");
      this->activeDynNestConfigMap.erase(args.seqNum);
      staticNestConfig.dynConfigs.pop_back();
      SE_DPRINTF("[Nest] Rewind DynNestConfig for region %s.\n",
                 nestRegion.region());
    }
  }
}

void NestStreamController::commitStreamEnd(const EndArgs &args) {
  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->se->getStreamRegion(infoRelativePath);
  for (const auto &nestRelativePath :
       streamRegion.nest_region_relative_paths()) {
    const auto &nestRegion = this->se->getStreamRegion(nestRelativePath);
    if (this->staticNestConfigMap.count(nestRegion.region())) {
      // Initialize a DynNestConfig.
      auto &staticNestConfig =
          this->staticNestConfigMap.at(nestRegion.region());
      assert(!staticNestConfig.dynConfigs.empty() && "Missing DynNestConfig.");
      const auto &dynNestConfig = staticNestConfig.dynConfigs.front();
      assert(dynNestConfig.seqNum < args.seqNum && "End before configured.");
      this->activeDynNestConfigMap.erase(dynNestConfig.seqNum);
      staticNestConfig.dynConfigs.pop_front();
      SE_DPRINTF(
          "[Nest] Release DynNestConfig for region %s, remaining %llu.\n",
          nestRegion.region(), staticNestConfig.dynConfigs.size());
    }
  }
}

void NestStreamController::configureNestStreams() {
  for (auto &entry : this->activeDynNestConfigMap) {
    if (entry.second->configExecuted) {
      this->configureNestStream(*entry.second);
    }
  }
}

void NestStreamController::configureNestStream(DynNestConfig &dynNestConfig) {

  /**
   * Since allocating a new stream will take one element, we check that
   * there are available free elements.
   */
  if (this->se->numFreeFIFOEntries <
      dynNestConfig.staticNestConfig->configStreams.size()) {
    SE_DPRINTF("[Nest] No Total Free Element to allocate NestConfig, Has %d, "
               "Required %d.\n",
               this->se->numFreeFIFOEntries,
               dynNestConfig.staticNestConfig->configStreams.size());
    return;
  }
  for (auto S : dynNestConfig.staticNestConfig->configStreams) {
    if (S->getAllocSize() + 1 >= S->maxSize) {
      // S_DPRINTF(S,
      //           "[Nest] No Free Element to allocate NestConfig, AllocSize %d,
      //           " "MaxSize %d.\n", S->getAllocSize(), S->maxSize);
      return;
    }
  }

  auto nextElementIdx = dynNestConfig.nextElementIdx;
  std::unordered_set<StreamElement *> baseElements;
  for (auto baseS : dynNestConfig.staticNestConfig->baseStreams) {
    auto &baseDynS = baseS->getDynamicStream(dynNestConfig.seqNum);
    auto baseElement = baseDynS.getElementByIdx(nextElementIdx);
    if (!baseElement) {
      if (baseDynS.FIFOIdx.entryIdx > nextElementIdx) {
        DYN_S_DPRINTF(baseDynS.dynamicStreamId,
                      "Failed to get element %llu for NestConfig. The "
                      "TotalTripCount must be 0. Skip.\n",
                      dynNestConfig.nextElementIdx);
        dynNestConfig.nextElementIdx++;
        return;
      } else {
        // The base element is not allocated yet.
        S_DPRINTF(baseS,
                  "[Nest] BaseElement not allocated yet for NestConfig.\n");
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

  // All base elements are value ready.
  auto getStreamValue = [&baseElements](uint64_t streamId) -> StreamValue {
    StreamValue ret;
    for (auto baseElement : baseElements) {
      if (!baseElement->getStream()->isCoalescedHere(streamId)) {
        continue;
      }
      baseElement->getValueByStreamId(streamId, ret.uint8Ptr(),
                                      sizeof(StreamValue));
      return ret;
    }
    panic("Failed to find base element.");
    return ret;
  };
  auto actualParams =
      convertFormalParamToParam(dynNestConfig.formalParams, getStreamValue);

  this->isaHandler.resetISAStreamEngine();
  auto configFuncStartSeqNum =
      dynNestConfig.getConfigSeqNum(dynNestConfig.nextElementIdx);
  dynNestConfig.configFunc->invoke(actualParams, &this->isaHandler,
                                   configFuncStartSeqNum);

  // Sanity check that nest streams have same TotalTripCount.
  bool isFirstDynS = true;
  int totalTripCount = 0;
  InstSeqNum configSeqNum = 0;
  for (auto S : dynNestConfig.staticNestConfig->configStreams) {
    auto &dynS = S->getLastDynamicStream();
    if (!dynS.hasTotalTripCount()) {
      S_PANIC(S, "NestStream must have TotalTripCount.");
    }
    if (isFirstDynS) {
      totalTripCount = dynS.getTotalTripCount();
      configSeqNum = dynS.configSeqNum;
      isFirstDynS = false;
    } else {
      if (totalTripCount != dynS.getTotalTripCount()) {
        S_PANIC(S, "NestStream has TotalTripCount %d, while others have %d.",
                dynS.getTotalTripCount(), totalTripCount);
      }
    }
  }

  /**
   * If the TotalTripCount is zero, we have to manually rewind the StreamConfig
   * immediately, as the core will not execute the StreamEnd.
   */
  SE_DPRINTF("[Nest] Value ready. Configure NestRegion %s, OuterElementIdx %llu, "
             "TotalTripCount %d, Configured DynStreams:\n",
             dynNestConfig.staticNestConfig->region.region(),
             dynNestConfig.nextElementIdx, totalTripCount);
  if (Debug::StreamNest) {
    for (auto S : dynNestConfig.staticNestConfig->configStreams) {
      auto &dynS = S->getLastDynamicStream();
      SE_DPRINTF("[Nest]   %s.\n", dynS.dynamicStreamId);
    }
  }
  if (totalTripCount == 0) {
    // Technically, the StreamConfig is already committed. But here we just
    // rewind it.
    StreamEngine::StreamConfigArgs args(
        configSeqNum, dynNestConfig.staticNestConfig->region.relative_path());
    this->se->rewindStreamConfig(args);
  }

  dynNestConfig.nextElementIdx++;
}

void NestStreamController::takeOverBy(GemForgeCPUDelegator *newCPUDelegator) {
  this->isaHandler.takeOverBy(newCPUDelegator);
}

InstSeqNum NestStreamController::DynNestConfig::getConfigSeqNum(
    uint64_t elementIdx) const {
  // We add 1 to NumInsts because we have to count StreamEnd.
  auto numInsts = this->configFunc->getNumInstructions();
  return this->seqNum + 1 + elementIdx * (numInsts + 1);
}

InstSeqNum
NestStreamController::DynNestConfig::getEndSeqNum(uint64_t elementIdx) const {
  auto numInsts = this->configFunc->getNumInstructions();
  return this->seqNum + 1 + elementIdx * (numInsts + 1) + numInsts;
}