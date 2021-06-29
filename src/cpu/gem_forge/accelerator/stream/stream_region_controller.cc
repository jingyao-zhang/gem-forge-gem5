#include "stream_region_controller.hh"

#include "base/trace.hh"
#include "debug/StreamRegion.hh"

#define DEBUG_TYPE StreamRegion
#include "stream_log.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamRegion, format, ##args)

StreamRegionController::StreamRegionController(StreamEngine *_se)
    : se(_se), isaHandler(_se->getCPUDelegator()) {}

StreamRegionController::~StreamRegionController() {}

void StreamRegionController::initializeRegion(
    const ::LLVM::TDG::StreamRegion &region) {

  this->staticRegionMap.emplace(std::piecewise_construct,
                                std::forward_as_tuple(region.region()),
                                std::forward_as_tuple(region));

  auto &staticRegion = this->staticRegionMap.at(region.region());

  this->initializeNestStreams(region, staticRegion);
  this->initializeStreamLoopBound(region, staticRegion);
}

void StreamRegionController::dispatchStreamConfig(const ConfigArgs &args) {
  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->se->getStreamRegion(infoRelativePath);
  auto &staticRegion = this->getStaticRegion(streamRegion.region());
  auto &dynRegion = this->pushDynRegion(staticRegion, args.seqNum);

  this->dispatchStreamConfigForNestStreams(args, dynRegion);
  this->dispatchStreamConfigForLoopBound(args, dynRegion);
}

void StreamRegionController::executeStreamConfig(const ConfigArgs &args) {
  if (this->activeDynRegionMap.count(args.seqNum)) {
    auto &dynRegion = *this->activeDynRegionMap.at(args.seqNum);
    this->executeStreamConfigForNestStreams(args, dynRegion);
    this->executeStreamConfigForLoopBound(args, dynRegion);

    SE_DPRINTF("[Region] Executed Config SeqNum %llu for region %s.\n",
               args.seqNum, dynRegion.staticRegion->region.region());

    dynRegion.configExecuted = true;
  }
}

void StreamRegionController::rewindStreamConfig(const ConfigArgs &args) {
  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->se->getStreamRegion(infoRelativePath);

  auto &staticRegion = this->getStaticRegion(streamRegion.region());
  assert(!staticRegion.dynRegions.empty() && "Missing DynRegion.");

  const auto &dynRegion = staticRegion.dynRegions.back();
  assert(dynRegion.seqNum == args.seqNum && "Mismatch in rewind seqNum.");

  this->activeDynRegionMap.erase(args.seqNum);
  staticRegion.dynRegions.pop_back();

  SE_DPRINTF("[Region] Rewind DynRegion for region %s.\n",
             streamRegion.region());
}

void StreamRegionController::commitStreamEnd(const EndArgs &args) {
  const auto &infoRelativePath = args.infoRelativePath;
  const auto &streamRegion = this->se->getStreamRegion(infoRelativePath);

  auto &staticRegion = this->getStaticRegion(streamRegion.region());
  assert(!staticRegion.dynRegions.empty() && "Missing DynRegion.");

  const auto &dynRegion = staticRegion.dynRegions.front();
  assert(dynRegion.seqNum < args.seqNum && "End before configured.");

  SE_DPRINTF(
      "[Region] Release DynRegion SeqNum %llu for region %s, remaining %llu.\n",
      dynRegion.seqNum, streamRegion.region(),
      staticRegion.dynRegions.size() - 1);

  this->activeDynRegionMap.erase(dynRegion.seqNum);
  staticRegion.dynRegions.pop_front();
}

void StreamRegionController::tick() {
  for (auto &entry : this->activeDynRegionMap) {
    auto &dynRegion = *entry.second;
    if (dynRegion.configExecuted) {
      for (auto &dynNestConfig : dynRegion.nestConfigs) {
        this->configureNestStream(dynRegion, dynNestConfig);
      }
      this->checkLoopBound(dynRegion);
    }
  }
}

void StreamRegionController::takeOverBy(GemForgeCPUDelegator *newCPUDelegator) {
  this->isaHandler.takeOverBy(newCPUDelegator);
}

StreamRegionController::DynRegion &
StreamRegionController::pushDynRegion(StaticRegion &staticRegion,
                                      uint64_t seqNum) {
  staticRegion.dynRegions.emplace_back(&staticRegion, seqNum);
  auto &dynRegion = staticRegion.dynRegions.back();
  assert(this->activeDynRegionMap
             .emplace(std::piecewise_construct, std::forward_as_tuple(seqNum),
                      std::forward_as_tuple(&dynRegion))
             .second &&
         "Multiple nesting is not supported.");

  SE_DPRINTF("[Region] Initialized DynRegion SeqNum %llu for region %s. "
             "Current %d.\n",
             seqNum, staticRegion.region.region(),
             staticRegion.dynRegions.size());
  return dynRegion;
}

StreamRegionController::StaticRegion &
StreamRegionController::getStaticRegion(const std::string &regionName) {
  auto iter = this->staticRegionMap.find(regionName);
  if (iter == this->staticRegionMap.end()) {
    panic("Failed to find StaticRegion %s.\n", regionName);
  }
  return iter->second;
}

void StreamRegionController::buildFormalParams(
    const ConfigArgs::InputVec &inputVec, int &inputIdx,
    const ::LLVM::TDG::ExecFuncInfo &funcInfo,
    DynamicStreamFormalParamV &formalParams) {
  for (const auto &arg : funcInfo.args()) {
    if (arg.is_stream()) {
      // This is a stream input.
      formalParams.emplace_back();
      auto &formalParam = formalParams.back();
      formalParam.isInvariant = false;
      formalParam.baseStreamId = arg.stream_id();
    } else {
      if (inputIdx >= inputVec.size()) {
        panic("Missing input for %s: Given %llu, inputIdx %d.", funcInfo.name(),
              inputVec.size(), inputIdx);
      }
      formalParams.emplace_back();
      auto &formalParam = formalParams.back();
      formalParam.isInvariant = true;
      formalParam.invariant = inputVec.at(inputIdx);
      inputIdx++;
    }
  }
}

StreamValue StreamRegionController::GetStreamValueFromElementSet::operator()(
    uint64_t streamId) const {
  StreamValue ret;
  for (auto baseElement : this->elements) {
    if (!baseElement->getStream()->isCoalescedHere(streamId)) {
      continue;
    }
    baseElement->getValueByStreamId(streamId, ret.uint8Ptr(),
                                    sizeof(StreamValue));
    return ret;
  }
  panic("%s Failed to find base element for stream %llu.", streamId);
  return ret;
}