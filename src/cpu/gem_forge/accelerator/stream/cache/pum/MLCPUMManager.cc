#include "MLCPUMManager.hh"
#include "PUMEngine.hh"

#include "../LLCStreamEngine.hh"
#include "../MLCStrandManager.hh"

#include "cpu/gem_forge/accelerator/stream/cache/CacheStreamConfigureData.hh"
#include "sim/stream_nuca/stream_nuca_manager.hh"
#include "sim/stream_nuca/stream_nuca_map.hh"

#include "debug/MLCStreamPUM.hh"

#define DEBUG_TYPE MLCStreamPUM
#include "../../stream_log.hh"

#define MLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(MLCStreamPUM, "[MLC_SE%d]: [PUM] " format,                           \
          this->controller->getMachineID().num, ##args)
#define MLCSE_PANIC(format, args...)                                           \
  panic("[MLC_SE%d]: [PUM] " format, this->controller->getMachineID().num,     \
        ##args)

int64_t MLCPUMManager::PUMContext::nextContextId = 0;

AffinePattern MLCPUMManager::PatternInfo::getPatternAdjustedByOuterIter(
    int64_t patternIdx, int64_t outerIter) const {
  assert(patternIdx < this->atomicPatterns.size());
  auto pattern = this->atomicPatterns.at(patternIdx);
  if (patternIdx >= this->splitOuterDims.size()) {
    // This is pattern is not splitted.
    return pattern;
  }
  const auto &outerPat = this->splitOuterDims.at(patternIdx);
  auto outerOffset = outerPat(outerIter);
  pattern.start += outerOffset;
  return pattern;
}

std::ostream &operator<<(std::ostream &os,
                         const MLCPUMManager::PUMDataGraphNode &node) {
  os << &node << " " << node.pumTile << " " << node.splitOutDim << " "
     << node.pattern << " = ";
  switch (node.type) {
  case MLCPUMManager::PUMDataGraphNode::TypeE::Value: {
    os << "Value " << node.regionName;
    break;
  }
  case MLCPUMManager::PUMDataGraphNode::TypeE::Sync: {
    os << "Sync";
    break;
  }
  case MLCPUMManager::PUMDataGraphNode::TypeE::Move: {
    assert(node.operands.size() == 1 && "Missing Operand for Move.");
    os << "Move " << node.operands.front() << " SrcPat " << node.sendPat;
    break;
  }
  case MLCPUMManager::PUMDataGraphNode::TypeE::Load: {
    os << "Load " << node.sendConfig->dynamicId << " SrcPat " << node.sendPat;
    break;
  }
  case MLCPUMManager::PUMDataGraphNode::TypeE::Compute: {
    os << "Cmp";
    for (const auto &operand : node.operands) {
      os << " " << operand;
    }
    break;
  }
  default: {
    panic("Not supported PUMGraphNodes.");
  }
  }
  return os;
}

std::string to_string(const MLCPUMManager::PUMDataGraphNode &node) {
  std::stringstream ss;
  ss << node;
  return ss.str();
}

MLCPUMManager::PUMDataGraphNode *MLCPUMManager::PUMDataGraphNode::newValueNode(
    const std::string &_regionName, const AffinePattern &_pumTile,
    const AffinePattern &_pattern, const AffinePattern &_splitOutDim,
    int _scalarElemSize, Addr _regionVAddr) {
  auto node = new PUMDataGraphNode(_regionName, TypeE::Value, _pumTile,
                                   _pattern, _splitOutDim, _scalarElemSize);
  node->regionVAddr = _regionVAddr;
  return node;
}

MLCPUMManager::PUMDataGraphNode *MLCPUMManager::PUMDataGraphNode::newMoveNode(
    const std::string &_regionName, const AffinePattern &_pumTile,
    const AffinePattern &_pattern, const AffinePattern &_splitOutDim,
    const AffinePattern &_sendPat, const AffinePattern &_sendSplitOutDim,
    PUMDataGraphNode *_sendNode, int _scalarElemSize) {
  auto node = new PUMDataGraphNode(_regionName, TypeE::Move, _pumTile, _pattern,
                                   _splitOutDim, _scalarElemSize);
  node->sendPat = _sendPat;
  node->sendSplitOutDim = _sendSplitOutDim;
  node->operands.push_back(_sendNode);
  _sendNode->users.push_back(node);
  return node;
}

MLCPUMManager::PUMDataGraphNode *MLCPUMManager::PUMDataGraphNode::newLoadNode(
    const std::string &_regionName, AffinePattern &_pumTile,
    const AffinePattern &_pattern, const AffinePattern &_splitOutDim,
    const AffinePattern &_sendPat, ConfigPtr _sendConfig, ConfigPtr _recvConfig,
    int _scalarElemSize) {
  auto node = new PUMDataGraphNode(_regionName, TypeE::Load, _pumTile, _pattern,
                                   _splitOutDim, _scalarElemSize);
  node->sendPat = _sendPat;
  node->sendConfig = _sendConfig;
  node->recvConfig = _recvConfig;
  return node;
}

MLCPUMManager::PUMDataGraphNode *MLCPUMManager::PUMDataGraphNode::newCmpNode(
    const std::string &_regionName, const AffinePattern &_pumTile,
    const AffinePattern &_pattern, const AffinePattern &_splitOutDim,
    int _scalarElemSize, ExecFuncPtr _func, PUMComputeStreamGroup *_group) {
  auto node = new PUMDataGraphNode(_regionName, TypeE::Compute, _pumTile,
                                   _pattern, _splitOutDim, _scalarElemSize);
  node->func = _func;
  node->group = _group;
  return node;
}

MLCPUMManager::PUMDataGraphNode *
MLCPUMManager::PUMDataGraphNode::newSyncNode() {
  auto node = new PUMDataGraphNode("nowhere", TypeE::Sync, AffinePattern(),
                                   AffinePattern(), AffinePattern(), 0);
  return node;
}

MLCPUMManager::PUMContext::~PUMContext() {
  this->clear();
  this->clearPUMDataGraphNodes();
}

MLCPUMManager::MLCPUMManager(MLCStreamEngine *_mlcSE)
    : mlcSE(_mlcSE), controller(_mlcSE->controller) {}

MLCPUMManager::~MLCPUMManager() {}

void MLCPUMManager::PUMContext::clear() {
  this->expectedAcksEverySync.clear();
  this->totalSentPackets = 0;
  this->totalRecvPackets = 0;
  this->receivedAcks = 0;
  this->reachedSync = 0;
  this->totalSyncs = 0;
  /**
   * Don't forget to release the commands.
   */
  this->commands.clear();
}

void MLCPUMManager::PUMContext::clearPUMDataGraphNodes() {
  for (auto node : this->pumDataGraphNodes) {
    delete node;
  }
  this->pumDataGraphNodes.clear();
}

void MLCPUMManager::findPUMComputeStreamGroups(PUMContext &context) {
  for (const auto &config : context.configs) {
    bool shouldFormGroup = false;
    CacheStreamConfigureDataPtr reduceConfig = nullptr;
    CacheStreamConfigureDataPtr realComputeConfig = config;
    if (config->stream->getEnabledStoreFunc()) {
      shouldFormGroup = true;
    } else if (config->stream->getEnabledLoadFunc()) {
      // We also try to support LoadCompute.
      shouldFormGroup = true;
    } else {
      /**
       * We try to support reduction here.
       */
      for (const auto &dep : config->depEdges) {
        if (dep.type == CacheStreamConfigureData::DepEdge::Type::UsedBy &&
            dep.data->stream->isReduction()) {
          shouldFormGroup = true;
          reduceConfig = dep.data;
          realComputeConfig = dep.data;
          break;
        }
      }
    }

    if (!shouldFormGroup) {
      continue;
    }

    MLC_S_DPRINTF(config->dynamicId, "[PUM] Form a PUMGroup.\n");
    context.pumGroups.emplace_back();
    auto &group = context.pumGroups.back();
    group.computeConfig = config;
    group.reduceConfig = reduceConfig;
    for (const auto &edge : realComputeConfig->baseEdges) {
      auto baseConfig = edge.data.lock();
      assert(baseConfig && "BaseConfig already released.");
      if (baseConfig == config) {
        // This is actually the RecvConfig.
        continue;
      }
      MLC_S_DPRINTF(baseConfig->dynamicId, "[PUM]   Added as UsedConfig.\n");
      group.usedConfigs.push_back(baseConfig);
    }
  }

  /**
   * Topological sort the groups.
   */
  std::unordered_map<const PUMComputeStreamGroup *, int> state;
  std::unordered_map<Stream *, const PUMComputeStreamGroup *> streamGroupMap;
  std::vector<const PUMComputeStreamGroup *> stack;
  std::vector<PUMComputeStreamGroup> sortedGroups;
  for (const auto &group : context.pumGroups) {
    stack.push_back(&group);
    state.emplace(&group, 0);
    streamGroupMap.emplace(group.computeConfig->stream, &group);
  }
  auto getGroup =
      [&streamGroupMap](
          const ConfigPtr &config) -> const PUMComputeStreamGroup * {
    if (streamGroupMap.count(config->stream)) {
      return streamGroupMap.at(config->stream);
    }
    return nullptr;
  };
  while (!stack.empty()) {
    auto group = stack.back();
    switch (state.at(group)) {
    case 0: {
      // first time.
      for (const auto &usedConfig : group->usedConfigs) {
        if (auto usedGroup = getGroup(usedConfig)) {
          stack.push_back(usedGroup);
          assert(state.emplace(usedGroup, 0).first->second != 1 &&
                 "Found Loop");
        }
      }
      state.at(group) = 1;
      break;
    }
    case 1: {
      // second time.
      sortedGroups.emplace_back(*group);
      state.at(group) = 2;
      stack.pop_back();
      break;
    }
    default: {
      // Already visited.
      stack.pop_back();
      break;
    }
    }
  }

  context.pumGroups.swap(sortedGroups);
}

bool MLCPUMManager::canApplyPUMToGroup(PUMContext &context,
                                       PUMComputeStreamGroup &group) {
  /**
   * We can only apply PUM iff:
   * 1. ComputeS has no DepEdge.
   * 2. All UsedByS has no BaseEdge (hence affine).
   * 3. Loop is eliminated.
   * 4. Known trip count.
   * 5. For stream patterns:
   *    StoreComputeStream must be a sub-region.
   *    LoadForwardStream must be able to reduce to a sub-region,
   *    with matched dimension with the StoreComputeStream.
   * 6. TODO: Enough wordlines to hold inputs and intermediate data.
   */

  const auto &computeConfig = group.computeConfig;
  const auto &groupDynId = computeConfig->dynamicId;

  MLC_S_DPRINTF(groupDynId, "[CheckCanPUM] -------------------.\n");

  if (!computeConfig->stream->isLoopEliminated()) {
    MLC_S_DPRINTF(groupDynId, "[NoPUM] Not Eliminated.\n");
    return false;
  }

  /**
   * Check some constraints on all streams: with the first one being
   * ComputeConfig.
   */
  CacheStreamConfigureVec allConfigs = group.usedConfigs;
  allConfigs.insert(allConfigs.begin(), computeConfig);
  for (const auto &config : allConfigs) {

    const auto &dynId = config->dynamicId;

    if (config->floatPlan.isFloatedToMem()) {
      MLC_S_DPRINTF(dynId, "[NoPUM] Float to Mem.\n");
      return false;
    }
    if (config->floatPlan.getFirstFloatElementIdx() != 0) {
      MLC_S_DPRINTF(dynId, "[NoPUM] Delayed Float.\n");
      return false;
    }

    if (!config->hasTotalTripCount()) {
      MLC_S_DPRINTF(dynId, "[NoPUM] No TripCount.\n");
      return false;
    }

    auto linearAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
        config->addrGenCallback);
    if (!linearAddrGen) {
      MLC_S_DPRINTF(dynId, "[NoPUM] Not LinearAddrGen.\n");
      return false;
    }

    /**
     * We first get the StreamNUCA region, to get the ScalarElemSize.
     */
    auto S = config->stream;
    auto cpuDelegator = S->getCPUDelegator();
    auto threadContext = cpuDelegator->getSingleThreadContext();
    auto streamNUCAManager = threadContext->getStreamNUCAManager();

    const auto &addrParams = config->addrGenFormalParams;
    auto startVAddr = linearAddrGen->getStartAddr(addrParams);
    const auto &streamNUCARegion =
        streamNUCAManager->getContainingStreamRegion(startVAddr);
    auto scalarElemSize = streamNUCARegion.elementSize;
    auto regionVAddr = streamNUCARegion.vaddr;

    Addr startPAddr;
    if (!cpuDelegator->translateVAddrOracle(startVAddr, startPAddr)) {
      MLC_S_DPRINTF(dynId, "[NoPUM] Fault StartVAddr.\n");
      return false;
    }
    auto rangeMap = StreamNUCAMap::getRangeMapContaining(startPAddr);
    if (!rangeMap) {
      MLC_S_DPRINTF(dynId, "[NoPUM] No RangeMap.\n");
      return false;
    }

    /**
     * For UsedConfigs, split them into PUM and NonPUM categories.
     * NOTE: NonPUMConfigs have no PatternInfo.
     */
    if (computeConfig == config) {
      if (!rangeMap->isStreamPUM) {
        MLC_S_DPRINTF(dynId, "[NoPUM] RangeMap not PUM.\n");
        return false;
      }
      const int64_t tripCountThreshold = 2048;
      if (config->getTotalTripCount() <= tripCountThreshold) {
        MLC_S_DPRINTF(dynId, "[NoPUM] TripCount %ld < Threshold %ld.\n",
                      tripCountThreshold);
        return false;
      }

    } else {
      if (rangeMap->isStreamPUM &&
          rangeMap->pumTile ==
              group.patternInfo.at(computeConfig->stream).pumTile) {
        MLC_S_DPRINTF(dynId, "[CheckCanPUM]   As UsedPUMConfig.\n");
        group.usedPUMConfigs.push_back(config);
      } else {
        MLC_S_DPRINTF(dynId, "[CheckCanPUM]   As UsedNonPUMConfig.\n");
        group.usedNonPUMConfigs.push_back(config);
      }
    }

#define AssertScalarAlign(v) assert((v) % scalarElemSize == 0)
#define AlignToScalarElem(v) ((v) / scalarElemSize)

    AssertScalarAlign(startVAddr - regionVAddr);
    auto scalarStart = AlignToScalarElem(startVAddr - regionVAddr);

    AffinePattern::ParamVecT params;
    assert(addrParams.size() % 2 == 1);
    int64_t prevTotalTrip = 1;
    for (auto i = 0; i + 1 < addrParams.size(); i += 2) {
      auto stride = addrParams[i].invariant.uint64();
      auto totalTrip = addrParams[i + 1].invariant.uint64();
      AssertScalarAlign(stride);
      auto scalarStride = AlignToScalarElem(stride);
      auto scalarTrip = totalTrip / prevTotalTrip;
      params.emplace_back(scalarStride, scalarTrip);

      prevTotalTrip = totalTrip;
    }

    AffinePattern pattern(scalarStart, params);
    auto &patternInfo =
        group.patternInfo
            .emplace(std::piecewise_construct, std::forward_as_tuple(S),
                     std::forward_as_tuple())
            .first->second;
    patternInfo.regionName = streamNUCARegion.name;
    patternInfo.pumTile = rangeMap->pumTile;
    patternInfo.pattern = pattern;
    patternInfo.regionVAddr = regionVAddr;
    patternInfo.scalarElemSize = scalarElemSize;
    patternInfo.atomicPatterns =
        this->decoalesceAndDevectorizePattern(config, pattern, scalarElemSize);
  }

  // All regions should have the same tile mapping.
  const auto &computeTile = group.patternInfo.at(computeConfig->stream).pumTile;
  for (const auto &baseConfig : group.usedPUMConfigs) {
    const auto &tile = group.patternInfo.at(baseConfig->stream).pumTile;
    if (tile != computeTile) {
      MLC_S_DPRINTF(groupDynId, "[NoPUM] Mismatch Tile %s and %s from %s.\n",
                    computeTile, tile, baseConfig->dynamicId);
      return false;
    }
  }

  // Before asking DataMoveCompiler, we need some preprocessing on the stream
  // patterns to at least try to make it suitable for PUM.
  this->preprocessPatternsInGroup(context, group);

  for (const auto &e : group.patternInfo) {
    for (const auto &p : e.second.atomicPatterns) {
      S_DPRINTF(e.first, "[PUM]   AtomicPat %s.\n", p);
    }
  }

  // Check for DataMoveCompiler.
  for (const auto &sendConfig : group.usedPUMConfigs) {

    const auto &sendDynId = sendConfig->dynamicId;
    auto S = sendConfig->stream;
    const auto &sendPatInfo = group.patternInfo.at(S);
    auto myTile = sendPatInfo.pumTile;

    MLC_S_DPRINTF(sendDynId, "[PUM] --- Can Compile DataMove. MyTile %s.\n",
                  myTile);
    DataMoveCompiler compiler(PUMHWConfiguration::getPUMHWConfig(), myTile);

    auto recvS = computeConfig->stream;
    const auto &recvPatternInfo = group.patternInfo.at(recvS);
    auto recvTile = recvPatternInfo.pumTile;

    if (recvTile != myTile) {
      // TODO: Handle different mapping of source and dest stream.
      MLC_S_DPRINTF(groupDynId,
                    "[NoPUM] Mismatch RecvTile %s and SendTile %s.\n", recvTile,
                    myTile);
      return false;
    }

    if (recvPatternInfo.atomicPatterns.size() != 1) {
      MLC_S_DPRINTF(groupDynId, "[NoPUM] Multi RecvPatterns.\n", recvTile,
                    myTile);
      return false;
    }
    const auto &recvPattern = recvPatternInfo.atomicPatterns.front();
    MLC_S_DPRINTF(groupDynId, "[PUM] RecvPattern %s.\n", recvPattern);

    for (const auto &myPattern : sendPatInfo.atomicPatterns) {
      MLC_S_DPRINTF(sendDynId, "[PUM] SendPattern %s.\n", myPattern);

      if (!compiler.canCompileStreamPair(myPattern, recvPattern)) {
        MLC_S_DPRINTF(groupDynId, "[NoPUM] Rejected by DataMoveCompiler.\n");
        return false;
      }
    }
  }

  return true;
}

void MLCPUMManager::buildPUMDataGraph(PUMContext &context) {
  for (auto &group : context.pumGroups) {
    if (group.appliedPUM) {
      this->buildPUMDataGraph(context, group);
    }
  }

  if (Debug::MLCStreamPUM) {
    MLCSE_DPRINTF("--------------------- PUMDataGraph Nodes.\n");
    for (const auto &node : context.pumDataGraphNodes) {
      MLCSE_DPRINTF("-- Node %s.\n", *node);
    }
  }

  this->mergePUMDataGraphMoveNode(context);

  if (Debug::MLCStreamPUM) {
    MLCSE_DPRINTF("--------------------- PUMDataGraph After Merge.\n");
    for (const auto &node : context.pumDataGraphNodes) {
      MLCSE_DPRINTF("-- Node %s.\n", *node);
    }
  }

  auto scheduledNodes = this->schedulePUMDataGraph(context);
  context.pumDataGraphNodes = scheduledNodes;

  if (Debug::MLCStreamPUM) {
    MLCSE_DPRINTF("--------------------- PUMDataGraph After Schedule.\n");
    for (const auto &node : context.pumDataGraphNodes) {
      MLCSE_DPRINTF("-- Node %s.\n", *node);
    }
  }
}

void MLCPUMManager::buildPUMDataGraph(PUMContext &context,
                                      PUMComputeStreamGroup &group) {
  assert(group.canApplyPUM);

  PUMDataGraphNodeVec dataMoveNodes;
  for (const auto &sendConfig : group.usedPUMConfigs) {
    this->buildPUMDataGraphMove(context, group, sendConfig, dataMoveNodes);
  }

  /**
   * LoadComputeStream also need to send to itself.
   */
  if (group.computeConfig->stream->getEnabledLoadFunc()) {
    this->buildPUMDataGraphMove(context, group, group.computeConfig,
                                dataMoveNodes);
  }

  /**
   * NonPUMUsedConfigs are represented as Load nodes.
   */
  for (const auto &sendConfig : group.usedNonPUMConfigs) {
    this->buildPUMDataGraphLoad(context, group, sendConfig, dataMoveNodes);
  }

  /**
   * Construct the compute node.
   */
  this->buildPUMDataGraphCompute(context, group, dataMoveNodes);
}

bool MLCPUMManager::needExpandReuse(PUMContext &context,
                                    const PUMComputeStreamGroup &group) {
  bool shouldExpandReuse = false;
  if (group.computeConfig->stream->getEnabledLoadFunc()) {
    if (!group.usedNonPUMConfigs.empty()) {
      shouldExpandReuse = true;
    }
  }
  return shouldExpandReuse;
}

AffinePattern MLCPUMManager::expandReusePat(const AffinePattern &pumTile,
                                            const AffinePattern &pat,
                                            AffinePattern &splitOutDim) {
  auto arraySizes = pumTile.getArraySize();
  auto startPos = AffinePattern::getArrayPosition(arraySizes, pat.start);
  auto ret = pat;
  int64_t accArraySize = 1;
  int64_t adjustedStart = 0;
  for (int dim = 0; dim < ret.params.size(); ++dim) {
    assert(dim < arraySizes.size());
    auto &p = ret.params.at(dim);
    if (p.stride == 0) {
      p.stride = accArraySize;
      startPos.at(dim) = 0;
    }
    adjustedStart += startPos.at(dim) * accArraySize;
    accArraySize *= arraySizes.at(dim);
  }
  ret.start = adjustedStart;
  // Clear all stride in split out dim so that they stay at same location.
  for (int dim = 0; dim < splitOutDim.params.size(); ++dim) {
    splitOutDim.params.at(dim).stride = 0;
  }
  return ret;
}

void MLCPUMManager::buildPUMDataGraphMove(PUMContext &context,
                                          PUMComputeStreamGroup &group,
                                          const ConfigPtr &sendConfig,
                                          PUMDataGraphNodeVec &resultNodes) {

  const auto &sendDynId = sendConfig->dynamicId;
  auto sendS = sendConfig->stream;
  const auto &sendPatInfo = group.patternInfo.at(sendS);
  auto sendTile = sendPatInfo.pumTile;

  auto recvConfig = group.computeConfig;
  auto recvS = recvConfig->stream;
  const auto &recvPatInfo = group.patternInfo.at(recvS);
  auto recvTile = recvPatInfo.pumTile;

  MLC_S_DPRINTF(recvConfig->dynamicId,
                "[PUM] --- Build DataMove Node. SendFrom %s SendTile %s.\n",
                sendConfig->dynamicId, sendTile);

  if (recvTile != sendTile) {
    // TODO: Handle different mapping of source and dest stream.
    MLC_S_PANIC_NO_DUMP(sendDynId, "[PUM] Different Tile.");
  }

  int64_t recvPatIdx = 0;
  if (recvConfig->stream->getEnabledLoadFunc()) {
    // This is a LoadCompute.
    recvPatIdx = recvPatInfo.getLoadComputeResultPatternIdx();
  } else {
    if (recvPatInfo.atomicPatterns.size() != 1) {
      MLC_S_PANIC_NO_DUMP(recvConfig->dynamicId, "[PUM] Multi Recv.");
    }
  }

  auto recvPat = recvPatInfo.getPattern(recvPatIdx);
  auto recvSplitOutDim = recvPatInfo.getSplitOutDim(recvPatIdx);
  MLC_S_DPRINTF(recvConfig->dynamicId, "[PUM] RecvPat %s RecvSplitOutDim %s.\n",
                recvPat, recvSplitOutDim);

  /**
   * Find the real SendPatterns.
   * 1. If LoadComputeStream sends to other Stream, we only need to send
   * LoadComputeResultPattern.
   * 2. If LoadComputeStream sends to itself, we need to:
   *  a. Send patterns other than LoadComputeResultPattern.
   *  b. Mark LoadComputeResultPattern as one Value.
   *
   * If this Computation involves NonPUMConfig, then we need to expand the
   * reused dimension.
   */
  bool shouldExpandReuse = this->needExpandReuse(context, group);
  if (shouldExpandReuse) {
    recvPat = expandReusePat(recvTile, recvPat, recvSplitOutDim);
  }

  for (auto patIdx = 0; patIdx < sendPatInfo.atomicPatterns.size(); ++patIdx) {

    if (sendS->getEnabledLoadFunc()) {
      auto sendLoadComputeResultPatIdx =
          sendPatInfo.getLoadComputeResultPatternIdx();
      if (recvConfig != sendConfig) {
        // Send to other stream.
        if (patIdx != sendLoadComputeResultPatIdx) {
          continue;
        }
      }
    }

    auto sendPat = sendPatInfo.getPattern(patIdx);
    auto sendSplitOutDim = sendPatInfo.getSplitOutDim(patIdx);

    MLC_S_DPRINTF(sendDynId, "[PUM] SendPat %s SendSplitOutDim %s.\n", sendPat,
                  sendSplitOutDim);

    /**
     * Each send pattern is a ValueNode, with RecvPat as MoveNode.
     */
    PUMDataGraphNode *valueNode = nullptr;
    if (sendS->getEnabledLoadFunc() && recvConfig != sendConfig) {
      // Search for the compute node.
      for (auto node : context.pumDataGraphNodes) {
        if (node->type == PUMDataGraphNode::TypeE::Compute &&
            node->pumTile == sendTile &&
            node->group->computeConfig == sendConfig) {
          valueNode = node;
        }
      }
      if (valueNode == nullptr) {
        panic("Failed to find PUMDataGraphNode for LoadComputeS.");
      }
    } else {
      valueNode = PUMDataGraphNode::newValueNode(
          sendPatInfo.regionName, sendTile, sendPat, sendSplitOutDim,
          sendPatInfo.scalarElemSize, sendPatInfo.regionVAddr);
      context.pumDataGraphNodes.push_back(valueNode);

      if (shouldExpandReuse) {
        AffinePattern expandedSendSplitOutDim;
        auto expandedSendPat =
            expandReusePat(sendTile, sendPat, expandedSendSplitOutDim);
        if (expandedSendPat != sendPat) {
          // Add a MoveNode to do the reuse expansion.
          auto expandReuseMoveNode = PUMDataGraphNode::newMoveNode(
              sendPatInfo.regionName, sendTile, expandedSendPat,
              expandedSendSplitOutDim, sendPat, sendSplitOutDim, valueNode,
              recvPatInfo.scalarElemSize);
          context.pumDataGraphNodes.push_back(expandReuseMoveNode);

          // This is our new ValueNode.
          valueNode = expandReuseMoveNode;
        }
      }
    }

    if (sendS->getEnabledLoadFunc() && recvConfig == sendConfig) {
      auto sendLoadComputeResultPatIdx =
          sendPatInfo.getLoadComputeResultPatternIdx();
      if (patIdx == sendLoadComputeResultPatIdx) {
        // We add the ValueNode as the ResultNode, since this requires no send.
        resultNodes.push_back(valueNode);
        continue;
      }
    }

    auto moveNode = PUMDataGraphNode::newMoveNode(
        recvPatInfo.regionName, recvTile, recvPat, recvSplitOutDim,
        valueNode->pattern, valueNode->splitOutDim, valueNode,
        recvPatInfo.scalarElemSize);
    context.pumDataGraphNodes.push_back(moveNode);
    resultNodes.push_back(moveNode);
  }
}

void MLCPUMManager::buildPUMDataGraphLoad(PUMContext &context,
                                          PUMComputeStreamGroup &group,
                                          const ConfigPtr &sendConfig,
                                          PUMDataGraphNodeVec &resultNodes) {

  const auto &sendDynId = sendConfig->dynamicId;
  auto sendS = sendConfig->stream;
  const auto &sendPatInfo = group.patternInfo.at(sendS);

  auto recvConfig = group.computeConfig;
  auto recvS = recvConfig->stream;
  const auto &recvPatInfo = group.patternInfo.at(recvS);
  auto recvTile = recvPatInfo.pumTile;

  MLC_S_DPRINTF(recvConfig->dynamicId,
                "[PUM] --- Build Load Node. SendFrom %s.\n",
                sendConfig->dynamicId);

  int64_t recvPatIdx = 0;
  if (recvConfig->stream->getEnabledLoadFunc()) {
    // This is a LoadCompute.
    recvPatIdx = recvPatInfo.getLoadComputeResultPatternIdx();
  } else {
    if (recvPatInfo.atomicPatterns.size() != 1) {
      MLC_S_PANIC_NO_DUMP(recvConfig->dynamicId, "[PUM] Multi Recv.");
    }
  }

  auto recvPat = recvPatInfo.getPattern(recvPatIdx);
  auto recvSplitOutDim = recvPatInfo.getSplitOutDim(recvPatIdx);
  MLC_S_DPRINTF(recvConfig->dynamicId, "[PUM] RecvPat %s RecvSplitOutDim %s.\n",
                recvPat, recvSplitOutDim);

  /**
   * Find the real SendPatterns.
   * 1. If LoadComputeStream sends to other Stream, we only need to send
   * LoadComputeResultPattern.
   * 2. If LoadComputeStream sends to itself, we need to:
   *  a. Send patterns other than LoadComputeResultPattern.
   *  b. Mark LoadComputeResultPattern as one Value.
   *
   * If this Computation involves NonPUMConfig, then we need to expand the
   * reused dimension.
   */
  bool shouldExpandReuse = this->needExpandReuse(context, group);
  if (shouldExpandReuse) {
    recvPat = expandReusePat(recvTile, recvPat, recvSplitOutDim);
  }

  for (auto patIdx = 0; patIdx < sendPatInfo.atomicPatterns.size(); ++patIdx) {

    if (sendS->getEnabledLoadFunc()) {
      auto sendLoadComputeResultPatIdx =
          sendPatInfo.getLoadComputeResultPatternIdx();
      if (recvConfig != sendConfig) {
        // Send to other stream.
        if (patIdx != sendLoadComputeResultPatIdx) {
          continue;
        }
      }
    }

    auto sendPat = sendPatInfo.getPattern(patIdx);
    auto sendSplitOutDim = sendPatInfo.getSplitOutDim(patIdx);

    MLC_S_DPRINTF(sendDynId, "[PUM] SendPat %s SendSplitOutDim %s.\n", sendPat,
                  sendSplitOutDim);

    /**
     * Each send pattern is a ValueNode, with RecvPat as MoveNode.
     */
    assert(recvConfig != sendConfig);
    assert(!sendS->getEnabledLoadFunc());

    auto loadNode = PUMDataGraphNode::newLoadNode(
        recvPatInfo.regionName, recvTile, recvPat, recvSplitOutDim, sendPat,
        sendConfig, recvConfig, recvPatInfo.scalarElemSize);
    context.pumDataGraphNodes.push_back(loadNode);
    resultNodes.push_back(loadNode);
  }
}

void MLCPUMManager::buildPUMDataGraphCompute(
    PUMContext &context, PUMComputeStreamGroup &group,
    const PUMDataGraphNodeVec &moveNodes) {

  const auto &config = group.computeConfig;
  const auto &dynId = config->dynamicId;

  auto &patInfo = group.patternInfo.at(config->stream);

  int64_t patternIdx = 0;
  if (config->stream->getEnabledLoadFunc()) {
    patternIdx = patInfo.getLoadComputeResultPatternIdx();
  } else {
    assert(patInfo.atomicPatterns.size() == 1);
  }

  auto pattern = patInfo.getPattern(patternIdx);
  auto splitOutDim = patInfo.getSplitOutDim(patternIdx);

  bool shouldExpandReuse = this->needExpandReuse(context, group);
  if (shouldExpandReuse) {
    pattern = expandReusePat(patInfo.pumTile, pattern, splitOutDim);
  }

  MLC_S_DPRINTF(dynId, "DataGraph Compute Tile %s Pat %s SplitOutDim %s.\n",
                patInfo.pumTile, pattern, splitOutDim);

  ExecFuncPtr func = nullptr;
  if (group.reduceConfig) {
    auto funcAddrGenCb = std::dynamic_pointer_cast<FuncAddrGenCallback>(
        group.reduceConfig->addrGenCallback);
    if (!funcAddrGenCb) {
      MLC_S_PANIC_NO_DUMP(group.reduceConfig->dynamicId,
                          "[PUM] Reduction should have FuncAddrGenCallback.");
    }
    func = funcAddrGenCb->getExecFunc();
  } else if (config->stream->getEnabledLoadFunc()) {
    // This is a LoadCompute.
    func = config->loadCallback;
  } else {
    func = config->storeCallback;
  }
  if (!func) {
    MLC_S_PANIC_NO_DUMP(dynId, "[PUM] Failed to find ComputeFunc.");
  }

  auto cmpNode = PUMDataGraphNode::newCmpNode(
      patInfo.regionName, patInfo.pumTile, pattern, splitOutDim,
      patInfo.scalarElemSize, func, &group);
  cmpNode->operands = moveNodes;
  for (auto moveNode : moveNodes) {
    moveNode->users.push_back(cmpNode);
  }
  context.pumDataGraphNodes.push_back(cmpNode);
}

void MLCPUMManager::mergePUMDataGraphMoveNode(PUMContext &context) {

  if (!this->controller->myParams->stream_pum_optimize_dfg) {
    MLCSE_DPRINTF("[PUM] Disabled DFG Optimization.\n");
    return;
  }

  auto &nodes = context.pumDataGraphNodes;

  /**
   * We try to merge two moves if they are both moving a ValueNode,
   * and the ValueNode has only bounda difference.
   */

  auto isMoveNode = [](PUMDataGraphNode *node) -> bool {
    return node->type == PUMDataGraphNode::TypeE::Move;
  };

  auto shouldMergeTwoValueMove = [](PUMDataGraphNode *moveA,
                                    PUMDataGraphNode *moveB) -> bool {
    auto valueA = moveA->operands.front();
    auto valueB = moveB->operands.front();
    if (valueA != valueB) {
      if (valueA->type != PUMDataGraphNode::TypeE::Value ||
          valueB->type != PUMDataGraphNode::TypeE::Value) {
        // If not the same node, we require them to be ValueNode.
        return false;
      }
      if (valueA->regionVAddr != valueB->regionVAddr) {
        return false;
      }
      if (valueB->users.size() != 1) {
        return false;
      }
    }

    const auto &patA = moveA->sendPat;
    const auto &patB = moveB->sendPat;
    if (patA.params.size() != patB.params.size()) {
      return false;
    }

    auto arraySizes = valueA->pumTile.getArraySize();
    if (!patA.isSubRegionToArraySize(arraySizes, true /* allow reuse */) ||
        !patB.isSubRegionToArraySize(arraySizes, true /* allow reuse */)) {
      return false;
    }

    // We also need to check that moving dimension completely match.
    const auto &recvA = moveA->pattern;
    auto startsA = AffinePattern::getArrayPosition(arraySizes, patA.start);
    auto recvStartsA = AffinePattern::getArrayPosition(arraySizes, recvA.start);

    auto moveDimA = -1;
    auto moveDistA = 0;
    for (int dim = 0; dim < startsA.size(); ++dim) {
      if (startsA.at(dim) != recvStartsA.at(dim)) {
        if (moveDimA != -1) {
          panic("Moving along multiple dimensions.");
        }
        moveDimA = dim;
        moveDistA = recvStartsA.at(dim) - startsA.at(dim);
      }
    }

    const auto &recvB = moveB->pattern;
    auto startsB = AffinePattern::getArrayPosition(arraySizes, patB.start);
    auto recvStartsB = AffinePattern::getArrayPosition(arraySizes, recvB.start);

    auto moveDimB = -1;
    auto moveDistB = 0;
    for (int dim = 0; dim < startsB.size(); ++dim) {
      if (startsB.at(dim) != recvStartsB.at(dim)) {
        if (moveDimB != -1) {
          panic("Moving along multiple dimensions.");
        }
        moveDimB = dim;
        moveDistB = recvStartsB.at(dim) - startsB.at(dim);
      }
    }
    if (moveDimA != moveDimB) {
      // Either nothing to move or move along different dimensions.
      return false;
    }
    if (moveDistA != moveDistB) {
      // Move by different amount.
      return false;
    }

    // Use a heuristic that the intersection should have 90% of both sub-region.
    auto patANoReuse = AffinePattern::removeReuseInSubRegion(arraySizes, patA);
    auto patBNoReuse = AffinePattern::removeReuseInSubRegion(arraySizes, patB);
    auto intersect = AffinePattern::intersectSubRegions(arraySizes, patANoReuse,
                                                        patBNoReuse);

    auto patATrip = patANoReuse.getTotalTrip();
    auto patBTrip = patBNoReuse.getTotalTrip();
    auto intersectTrip = intersect.getTotalTrip();
    if (intersectTrip < patATrip * 0.9 || intersectTrip < patBTrip * 0.9) {
      // Not too much benefits to merge them
      return false;
    }

    return true;
  };

  auto mergeTwoValueMove = [&nodes](PUMDataGraphNode *moveA,
                                    PUMDataGraphNode *moveB) -> void {
    auto valueA = moveA->operands.front();
    auto valueB = moveB->operands.front();

    const auto &patA = moveA->sendPat;
    const auto &patB = moveB->sendPat;

    auto arraySizes = valueA->pumTile.getArraySize();

    // Merge them together.
    auto patANoReuse = AffinePattern::removeReuseInSubRegion(arraySizes, patA);
    auto patBNoReuse = AffinePattern::removeReuseInSubRegion(arraySizes, patB);
    auto mergedSrc =
        AffinePattern::unionSubRegions(arraySizes, patANoReuse, patBNoReuse);
    auto mergedDst = AffinePattern::unionSubRegions(arraySizes, moveA->pattern,
                                                    moveB->pattern);

    // Add back the reuse dimension.
    for (int dim = 0; dim < patA.params.size(); ++dim) {
      const auto &p = patA.params.at(dim);
      if (p.stride == 0) {
        mergedSrc.params.at(dim).stride = 0;
        mergedSrc.params.at(dim).trip = p.trip;
      }
    }

    moveA->pattern = mergedDst;
    moveA->sendPat = mergedSrc;

    moveB->replaceUsedBy(moveA);

    if (valueA != valueB) {
      // They must be value nodes.
      valueA->pattern = mergedSrc;

      bool erasedValueB = false;
      for (auto iter = nodes.begin(); iter != nodes.end(); ++iter) {
        if (*iter == valueB) {
          nodes.erase(iter);
          erasedValueB = true;
          break;
        }
      }
      assert(erasedValueB);
      delete valueB;
    }

    // Note: Now we remove moveB and valueB from nodes.
    bool erasedMoveB = false;
    for (auto iter = nodes.begin(); iter != nodes.end(); ++iter) {
      if (*iter == moveB) {
        nodes.erase(iter);
        erasedMoveB = true;
        break;
      }
    }
    assert(erasedMoveB);
    delete moveB;
  };

  while (true) {
    bool merged = false;
    for (int i = 0; i < nodes.size() && !merged; ++i) {
      auto nodeI = nodes.at(i);
      if (!isMoveNode(nodeI)) {
        continue;
      }

      for (int j = i + 1; j < nodes.size() && !merged; ++j) {
        auto nodeJ = nodes.at(j);
        if (!isMoveNode(nodeJ)) {
          continue;
        }

        if (!shouldMergeTwoValueMove(nodeI, nodeJ)) {
          continue;
        }

        // NOTE: This will erase NodeJ and ValueJ from nodes.
        mergeTwoValueMove(nodeI, nodeJ);
        merged = true;
      }
    }
    if (!merged) {
      break;
    }
  }
}

MLCPUMManager::PUMDataGraphNodeVec
MLCPUMManager::schedulePUMDataGraph(PUMContext &context) {
  if (this->controller->myParams->stream_pum_optimize_dfg) {
    return this->schedulePUMDataGraphBFS(context);
  } else {
    return this->schedulePUMDataGraphLinear(context);
  }
}

MLCPUMManager::PUMDataGraphNodeVec
MLCPUMManager::schedulePUMDataGraphLinear(PUMContext &context) {

  /**
   * This is used when we don't optimize the DFG. Here we simply
   * insert Sync node before and after CMP node.
   */

  PUMDataGraphNodeVec scheduledNodes;

  for (auto node : context.pumDataGraphNodes) {
    if (node->type != PUMDataGraphNode::TypeE::Compute) {
      scheduledNodes.push_back(node);
      continue;
    }
    // Check if the previous node is Sync.
    if (!scheduledNodes.empty() &&
        scheduledNodes.back()->type != PUMDataGraphNode::TypeE::Sync) {
      scheduledNodes.push_back(PUMDataGraphNode::newSyncNode());
    }
    scheduledNodes.push_back(node);
    // Insert a sync after that.
    scheduledNodes.push_back(PUMDataGraphNode::newSyncNode());
  }

  return scheduledNodes;
}

MLCPUMManager::PUMDataGraphNodeVec
MLCPUMManager::schedulePUMDataGraphBFS(PUMContext &context) {

  PUMDataGraphNodeVec scheduledNodes;

  std::set<PUMDataGraphNode *> scheduled;
  std::set<PUMDataGraphNode *> frontier;
  for (auto node : context.pumDataGraphNodes) {
    if (node->operands.empty()) {
      frontier.insert(node);
      scheduled.insert(node);
      scheduledNodes.push_back(node);
    }
  }

  while (!frontier.empty()) {
    std::set<PUMDataGraphNode *> nextFrontier;
    for (auto node : frontier) {
      for (auto user : node->users) {
        bool allOperandsScheduled = true;
        for (auto operand : user->operands) {
          if (!scheduled.count(operand)) {
            allOperandsScheduled = false;
            break;
          }
        }
        if (allOperandsScheduled) {
          nextFrontier.insert(user);
        }
      }
    }
    for (auto node : nextFrontier) {
      scheduledNodes.push_back(node);
      scheduled.insert(node);
    }
    // Insert a sync node.
    frontier = nextFrontier;
    if (!frontier.empty()) {
      scheduledNodes.push_back(PUMDataGraphNode::newSyncNode());
    }
  }

  return scheduledNodes;
}

void MLCPUMManager::compilePUMDataGraphToCommands(PUMContext &context) {

  for (const auto &group : context.pumGroups) {
    if (!group.appliedPUM) {
      continue;
    }
    if (context.nextOutIter != group.nextOuterIter) {
      panic("Mismatch in NextOutIter is not handled yet.");
    }
  }

  for (auto node : context.pumDataGraphNodes) {
    switch (node->type) {
    case PUMDataGraphNode::TypeE::Value:
    case PUMDataGraphNode::TypeE::Load: {
      // Value has nothing to compile for.
      // Load node is already offloaded as special PUMLoadStream.
      break;
    }
    case PUMDataGraphNode::TypeE::Move: {
      // This is data move.
      this->compileDataMove(context, node);
      break;
    }
    case PUMDataGraphNode::TypeE::Sync: {
      // This is sync node.
      this->compileSync(context, node);
      break;
    }
    case PUMDataGraphNode::TypeE::Compute: {
      // This is compute node.
      this->compileCompute(context, node);
      break;
    }
    default: {
      panic("Don't know how to compile PUMDataGraphNode %d.\n", node->type);
    }
    }
  }

  /**
   * We try to increment the OuterIter.
   */
  for (auto &group : context.pumGroups) {
    if (!group.appliedPUM) {
      continue;
    }
    group.nextOuterIter++;
  }
  context.nextOutIter++;
}

void MLCPUMManager::compileDataMove(PUMContext &context,
                                    PUMDataGraphNode *node) {

  auto sendNode = node->operands.front();
  const auto &sendTile = sendNode->pumTile;
  auto sendPat = node->adjustSendPatByOutIter(context.nextOutIter);
  const auto &recvTile = node->pumTile;
  auto recvPat = node->adjustPatByOutIter(context.nextOutIter);

  DataMoveCompiler compiler(PUMHWConfiguration::getPUMHWConfig(), sendTile);

  MLCSE_DPRINTF("[PUM] --- Compile MoveNode. SendTile %s %s -> %s %s.\n",
                sendTile, sendPat, recvTile, recvPat);

  if (recvTile != sendTile) {
    // TODO: Handle different mapping of source and dest stream.
    panic("[PUM] Different Tile.");
  }

  auto commands = compiler.compile(sendPat, recvPat);
  // Generate the meta information.
  for (auto &cmd : commands) {
    cmd.wordline_bits = node->scalarElemSize * 8;
    cmd.srcRegion = sendNode->regionName;
    cmd.srcAccessPattern = sendPat;
    cmd.srcMapPattern = sendTile;
    cmd.dstRegion = node->regionName;
    cmd.dstAccessPattern = recvPat;
    cmd.dstMapPattern = sendTile;
  }
  if (Debug::MLCStreamPUM) {
    for (const auto &command : commands) {
      MLCSE_DPRINTF("%s", command);
    }
  }
  context.commands.insert(context.commands.end(), commands.begin(),
                          commands.end());
}

void MLCPUMManager::compileSync(PUMContext &context, PUMDataGraphNode *node) {
  /**
   * Insert sync command after all data movement.
   * This is default enabled for every LLC bank.
   * NOTE: If there is no commands so far, we don't sync.
   */
  if (context.commands.empty()) {
    return;
  }
  context.commands.emplace_back();
  context.commands.back().type = "sync";
}

void MLCPUMManager::compileCompute(PUMContext &context,
                                   PUMDataGraphNode *node) {

  // const auto &config = group.computeConfig;
  // const auto &dynId = config->dynamicId;

  // auto &patInfo = group.patternInfo.at(config->stream);
  // auto scalarElemBits = patInfo.scalarElemSize * 8;

  DataMoveCompiler compiler(PUMHWConfiguration::getPUMHWConfig(),
                            node->pumTile);

  PUMCommandVecT commands;

  auto pattern = node->adjustPatByOutIter(context.nextOutIter);
  MLCSE_DPRINTF("Compile Compute Tile %s Pattern %s.\n", node->pumTile,
                pattern);

  ExecFuncPtr func = node->func;

  for (const auto &inst : func->getStaticInsts()) {

    commands.emplace_back();
    auto &command = commands.back();
    command.type = "cmp";
    command.opClass = inst->opClass();
    // Default bitline_mask is for the entire tile.
    command.bitline_mask = AffinePattern::constructSubRegion(
        compiler.tile_sizes,
        AffinePattern::IntVecT(compiler.tile_sizes.size(), 0) /* starts */,
        compiler.tile_sizes);

    MLCSE_DPRINTF("[PUM] Compile Inst %s to OpClass %s.\n",
                  inst->disassemble(0x0),
                  Enums::OpClassStrings[inst->opClass()]);
  }

  // Compile the final reduction instruction.
  this->compileReduction(context, *node->group, commands);

  if (Debug::MLCStreamPUM) {
    for (const auto &command : commands) {
      MLCSE_DPRINTF("%s", command);
    }
  }
  MLCSE_DPRINTF("Before masked.\n");

  // Mask the commands by the Stream.
  commands = compiler.maskCmdsBySubRegion(commands, pattern);

  if (Debug::MLCStreamPUM) {
    for (const auto &command : commands) {
      MLCSE_DPRINTF("%s", command);
    }
  }
  MLCSE_DPRINTF("Before mapped to LLC.\n");

  // Generate mask for each LLC bank.
  commands = compiler.mapCmdsToLLC(commands);

  // Generate the meta information.
  for (auto &cmd : commands) {
    cmd.wordline_bits = node->scalarElemSize * 8;
    cmd.srcRegion = node->regionName;
    cmd.srcAccessPattern = pattern;
    cmd.srcMapPattern = node->pumTile;
  }
  if (Debug::MLCStreamPUM) {
    for (const auto &command : commands) {
      MLCSE_DPRINTF("%s", command);
    }
  }

  context.commands.insert(context.commands.end(), commands.begin(),
                          commands.end());
}

void MLCPUMManager::compileReduction(PUMContext &context,
                                     PUMComputeStreamGroup &group,
                                     PUMCommandVecT &commands) {

  if (!group.reduceConfig) {
    return;
  }

  /**
   * We compile reduction into these steps:
   * 1. Check which dimension we are trying to reduce by looking at the inner
   * dimension stride of the assicated AffineStream. So far we only support
   * reduction over one dimension.
   *
   * 2. We define the following variables:
   *   InitElems: Initial # of elements in each tile waiting to be reduced.
   *   FinalElems: Final # of elements in each tile to be collected.
   * Notice that both InitElems and FinalElems will be power of 2, as we pick
   * the tiling factor to be power of 2.
   *
   * 3. We will generate these command sequence:
   *   Shift -> Reduce -> Shift -> Reduce -> ... -> Shift -> Reduce
   *
   * 4. Finally, the LLC PUMEngine will ready out FinalElems out and reduce
   * across its SRAM arrays. It then send back the results to the MLCPUMManager
   * for final reduction.
   */

  // 1. We assume reduction in the inner-most dimension.
  const auto &reduceDynId = group.reduceConfig->dynamicId;

  const auto &patInfo = group.patternInfo.at(group.computeConfig->stream);
  assert(!patInfo.atomicPatterns.empty() && "No AtomicPattern.");
  const auto &reducePat = patInfo.atomicPatterns.front();

  int reduceStride = reducePat.params.front().stride;
  auto ret = patInfo.pumTile.getTileAndArraySize();
  auto tileSizes = std::move(ret.first);
  auto arraySizes = std::move(ret.second);
  const auto &dimensions = arraySizes.size();
  int reduceDim = -1;
  int64_t curDimStride = 1;
  for (int dim = 0; dim < dimensions; ++dim) {
    if (curDimStride == reduceStride) {
      reduceDim = dim;
      break;
    }
  }
  if (reduceDim == -1) {
    MLC_S_PANIC_NO_DUMP(reduceDynId, "[PUMReduce] Failed to find ReduceDim.");
  }

  /**
   * As a hack for now, mark the last command reduction.
   * TODO: Truly slice the function and find real reduction instructions.
   */
  assert(!commands.empty() && "No Commands for Reduction.");

  // Let's take a copy of the final computation command.
  auto reduceCmd = commands.back();
  commands.pop_back();

  int64_t finalElems = 1;
  int64_t initElems = 1;
  for (int dim = 0; dim < dimensions; ++dim) {
    initElems *= tileSizes[dim];
    if (dim == reduceDim) {
      continue;
    }
    finalElems *= tileSizes[dim];
  }
  int64_t reduceRatio = tileSizes[reduceDim];
  assert(reduceRatio > 1 && "Nothing to reduce.");
  if (reduceRatio & (reduceRatio - 1)) {
    MLC_S_PANIC_NO_DUMP(reduceDynId,
                        "[PUMReduce] ReduceRatio %ld Not Power of 2.",
                        reduceRatio);
  }

  int64_t curRatio = 2;
  int64_t baseDist = AffinePattern::reduce_mul(
      tileSizes.begin(), tileSizes.begin() + reduceDim + 1, 1);
  MLC_S_DPRINTF(reduceDynId,
                "[PUMReduce] Dim %d BaseDist %ld ReduceRatio %ld Tile %s.\n",
                reduceDim, baseDist, reduceRatio, patInfo.pumTile);
  while (curRatio <= reduceRatio) {
    int64_t curDist = baseDist / curRatio;
    MLC_S_DPRINTF(reduceDynId, "[PUMReduce] CurRatio %ld ShiftDist %ld.\n",
                  curRatio, curDist);

    commands.emplace_back();
    commands.back().type = "intra-array";
    commands.back().bitline_dist = -curDist;

    // Generate the bitline mask.
    auto moveSizes = tileSizes;
    moveSizes.at(reduceDim) /= curRatio;
    auto moveStarts = AffinePattern::IntVecT(tileSizes.size(), 0);
    moveStarts.at(reduceDim) = moveSizes.at(reduceDim);

    commands.back().bitline_mask =
        AffinePattern::constructSubRegion(tileSizes, moveStarts, moveSizes);

    MLC_S_DPRINTF(reduceDynId, "Intra-Array Reduce Cmd %s", commands.back());

    // We then insert the reduce compute command.
    commands.push_back(reduceCmd);

    // Fix the bitline mask for the reduce commd.
    auto reduceStarts = AffinePattern::IntVecT(tileSizes.size(), 0);
    commands.back().bitline_mask =
        AffinePattern::constructSubRegion(tileSizes, reduceStarts, moveSizes);

    curRatio *= 2;
  }
}

void MLCPUMManager::compileContext(PUMContext &context) {

  this->compilePUMDataGraphToCommands(context);

  // Remember number of syncs.
  for (const auto &command : context.commands) {
    if (command.type == "sync") {
      context.totalSyncs++;
    }
  }

  // For now, we expect one Ack from each bank per sync.
  assert(context.expectedAcksEverySync.empty());
  auto totalBanks =
      this->controller->getNumCols() * this->controller->getNumRows();
  for (int i = 0; i < context.totalSyncs; ++i) {
    context.expectedAcksEverySync.push_back(totalBanks);
  }

  // The first sync will have extra Sync from each strand of LoadNode.
  assert(!context.expectedAcksEverySync.empty());
  for (const auto &node : context.pumDataGraphNodes) {
    if (node->type != PUMDataGraphNode::TypeE::Load) {
      continue;
    }

    const auto &sendConfig = node->sendConfig;
    auto mlcS =
        this->mlcSE->getStreamFromStrandId(DynStrandId(sendConfig->dynamicId));
    assert(mlcS && "Failed to find MLC SendS.");

    context.expectedAcksEverySync.front() +=
        mlcS->getDynStrandId().totalStrands;
  }
}

void MLCPUMManager::runPrefetchStage(PUMContext &context,
                                     CacheStreamConfigureVec *configs) {
  assert(configs != nullptr && !configs->empty());
  assert(context.savedPkt);

  MLCSE_DPRINTF("Starting prefetch stage.\n");

  // Dispatch & track prefetch streams.
  auto configsCpy = *configs;
  this->dispatchStreamConfigs(configs, context.savedPkt->req->masterId());

  this->inFlightPrefetchStreams = 0;
  this->totalSentPrefetchPkts = 0;
  this->totalRecvPrefetchPkts = 0;
  for (const auto &config : configsCpy) {
    this->inFlightPrefetchStreams += config->totalStrands;
    MLC_S_DPRINTF(config->dynamicId, "Prefetch strand count %d.\n",
                  config->totalStrands);
  }
}

void MLCPUMManager::runPUMExecutionStage(PUMContext &context) {
  assert(context.savedPkt);

  MLCSE_DPRINTF("Starting PUM execution stage.\n");

  // Configure any normal streams.
  auto normalConfigs = *(context.savedPkt->getPtr<CacheStreamConfigureVec *>());
  if (normalConfigs->empty()) {
    MLCSE_DPRINTF("Everything handled by PUM. No Normal Streams.\n");
  } else {
    this->dispatchStreamConfigs(normalConfigs,
                                context.savedPkt->req->masterId());
  }
  // Done with packet. Free it!
  delete context.savedPkt;
  context.savedPkt = nullptr;

  // Finish configuring PUM.
  this->postMLCSEConfigure(context);
}

void MLCPUMManager::runMLCConfigWithoutPUM(PacketPtr pkt) {
  auto normalConfigs = *(pkt->getPtr<CacheStreamConfigureVec *>());
  assert(!normalConfigs->empty());

  this->dispatchStreamConfigs(normalConfigs, pkt->req->masterId());
  // Done with packet. Free it!
  delete pkt;
}

void MLCPUMManager::dispatchStreamConfigs(CacheStreamConfigureVec *configs,
                                          MasterID masterId) const {
  assert(!configs->empty());

  this->mlcSE->strandManager->receiveStreamConfigure(configs, masterId);
  if (this->mlcSE->controller->isStreamRangeSyncEnabled()) {
    // Enable the range check.
    this->mlcSE->scheduleEvent(Cycles(1));
  }
}

CacheStreamConfigureVec
MLCPUMManager::generatePrefetchStreams(PUMComputeStreamGroup &group) {
  assert(group.canApplyPUM);

  auto genPrefetchStream =
      [&](const ConfigPtr &config) -> CacheStreamConfigureDataPtr {
    // Get NUCA region.
    auto S = config->stream;
    auto cpuDelegator = S->getCPUDelegator();
    auto threadContext = cpuDelegator->getSingleThreadContext();
    auto streamNUCAManager = threadContext->getStreamNUCAManager();

    auto linearAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
        config->addrGenCallback);
    const auto &addrParams = config->addrGenFormalParams;
    auto startVAddr = linearAddrGen->getStartAddr(addrParams);
    const auto &streamNUCARegion =
        streamNUCAManager->getContainingStreamRegion(startVAddr);

    // If this is not transposed, done.
    Addr regionPAddr;
    if (!cpuDelegator->translateVAddrOracle(streamNUCARegion.vaddr,
                                            regionPAddr)) {
      assert(false && "[PUM] Prefetch unable to translate VA->PA");
    }
    auto &nucaMapEntry = StreamNUCAMap::getRangeMapByStartPAddr(regionPAddr);
    if (!nucaMapEntry.isStreamPUM) {
      return std::shared_ptr<CacheStreamConfigureData>(nullptr);
    }

    if (streamNUCARegion.name == "gfm.conv3d.k") {
      // Avoid prefetching for the kernel weight in conv3d.
      return std::shared_ptr<CacheStreamConfigureData>(nullptr);
    }

    // If cached, done :)
    if (nucaMapEntry.isCached) {
      return std::shared_ptr<CacheStreamConfigureData>(nullptr);
    }

    MLCSE_DPRINTF("Prefetch stream %s.\n", config->dynamicId);

    /**
     * ! Hack: Assume prefetch streams will fetch the whole region.
     * This avoids us creating multiple prefetch streams for one region.
     */
    nucaMapEntry.isCached = true;

    // Otherwise, generate and issue prefetch stream.
    auto prefetchConfig = std::make_shared<CacheStreamConfigureData>(*config);
    prefetchConfig->isPUMPrefetch = true;

    // Clear dependencies.
    prefetchConfig->clearEdges();
    for (auto &param : prefetchConfig->storeFormalParams) {
      param.invariant.uint64() = 0;
      param.isInvariant = true;
    }
    for (auto &param : prefetchConfig->loadFormalParams) {
      param.invariant.uint64() = 0;
      param.isInvariant = true;
    }

    // Generate address.
    prefetchConfig->initVAddr = streamNUCARegion.vaddr;
    prefetchConfig->initPAddr = regionPAddr;
    prefetchConfig->initPAddrValid = true;

    // Opt: prefetch patterns only describes an element from each of the
    // required cache-lines.
    auto clSize = RubySystem::getBlockSizeBytes();
    prefetchConfig->elementSize = clSize;

    auto totalBytes =
        streamNUCARegion.numElement * streamNUCARegion.elementSize;
    auto numCLs =
        (totalBytes + streamNUCARegion.vaddr % clSize + clSize - 1) / clSize;

    prefetchConfig->addrGenFormalParams.clear();

    // Stride.
    prefetchConfig->addrGenFormalParams.emplace_back();
    prefetchConfig->addrGenFormalParams.back().invariant.uint64() = clSize;
    prefetchConfig->addrGenFormalParams.back().isInvariant = true;

    // Trip count.
    prefetchConfig->addrGenFormalParams.emplace_back();
    prefetchConfig->addrGenFormalParams.back().invariant.uint64() = numCLs;
    prefetchConfig->addrGenFormalParams.back().isInvariant = true;

    // Starting.
    prefetchConfig->addrGenFormalParams.emplace_back();
    prefetchConfig->addrGenFormalParams.back().invariant.uint64() =
        prefetchConfig->initVAddr;
    prefetchConfig->addrGenFormalParams.back().isInvariant = true;

    // Fix the TripCount in config.
    prefetchConfig->totalTripCount = numCLs;

    /**
     * Set the float plan to offload to the LLC controller.
     */
    uint64_t firstMemFloatElemIdx = 0;
    prefetchConfig->floatPlan = StreamFloatPlan();
    prefetchConfig->floatPlan.addFloatChangePoint(firstMemFloatElemIdx,
                                                  MachineType_Directory);
    prefetchConfig->floatPlan.finalize();

    return prefetchConfig;
  };

  // Fetch for compute.
  CacheStreamConfigureVec configs;

#define ADD_PREFETCH_STREAM(stream)                                            \
  {                                                                            \
    auto pStream = genPrefetchStream(stream);                                  \
    if (pStream != nullptr) {                                                  \
      configs.emplace_back(pStream);                                           \
    }                                                                          \
  }

  // Generate for compute stream.
  // TODO: Potential optimization since this is not necessary if the entire
  // region will be overwritten
  ADD_PREFETCH_STREAM(group.computeConfig);

  // Generate for dependencies.
  for (const auto &baseConfig : group.usedConfigs)
    ADD_PREFETCH_STREAM(baseConfig);

#undef ADD_PREFETCH_STREAM

  return configs;
}

void MLCPUMManager::notifyPrefetchStreamComplete(int64_t numSentPkts) {
  this->totalSentPrefetchPkts += numSentPkts;
  MLCSE_DPRINTF(
      "Prefetch stream complete (%d remaining). SentPrefetchPkt + %ld = %ld.\n",
      this->inFlightPrefetchStreams - 1, numSentPkts,
      this->totalSentPrefetchPkts);
  this->inFlightPrefetchStreams--;
  if (this->inFlightPrefetchStreams == 0 &&
      this->totalRecvPrefetchPkts == this->totalSentPrefetchPkts) {
    assert(!this->contexts.empty() && "There is no context to be prefetched.");
    auto &context = this->contexts.front();
    this->finishPrefetchStream(context);
  }
}

void MLCPUMManager::receivePrefetchPacket(int recvPackets) {
  this->totalRecvPrefetchPkts += recvPackets;
  // MLCSE_DPRINTF("Recv Prefetch Data + %ld = %ld.\n", recvPackets,
  //               this->totalRecvPrefetchPkts);
  if (this->inFlightPrefetchStreams == 0 &&
      this->totalRecvPrefetchPkts == this->totalSentPrefetchPkts) {
    assert(!this->contexts.empty() && "There is no context to be prefetched.");
    auto &context = this->contexts.front();
    this->finishPrefetchStream(context);
  }
}

void MLCPUMManager::finishPrefetchStream(PUMContext &context) {
  MLCSE_DPRINTF("Completed prefetch stage.\n");

  // Release all prefetch streams in the MLC SE.
  assert(context.savedPkt);
  auto masterId = context.savedPkt->req->masterId();
  std::vector<DynStreamId> prefetchDynIds;
  for (const auto &prefetchConfig : context.prefetchConfigs) {
    prefetchDynIds.push_back(prefetchConfig->dynamicId);
  }
  this->mlcSE->strandManager->receiveStreamEnd(prefetchDynIds, masterId);

  // Record the prefetch cycles.
  auto prefetchCycles = this->controller->curCycle() - context.initCycle;
  this->controller->m_statPUMPrefetchCycles += prefetchCycles;
  runPUMExecutionStage(context);
}

void MLCPUMManager::erasePUMConfigs(PUMContext &context,
                                    CacheStreamConfigureVec *configs,
                                    const PUMComputeStreamGroup &group) {

  auto eraseFromNormalConfigs = [&](const ConfigPtr &target) -> void {
    for (auto iter = configs->begin(); iter != configs->end(); ++iter) {
      const auto &config = *iter;
      if (config == target) {
        context.purePUMStreamIds.push_back(target->dynamicId);
        configs->erase(iter);
        return;
      }
    }
  };

  if (!group.appliedPUM) {
    return;
  }

  MLC_S_DPRINTF(group.computeConfig->dynamicId,
                "[PUMErased] Erase ComputeConfig.\n");
  eraseFromNormalConfigs(group.computeConfig);

  for (const auto &sendConfig : group.usedConfigs) {
    auto &deps = sendConfig->depEdges;
    bool erased = false;
    for (auto iter = deps.begin(); iter != deps.end(); ++iter) {
      if (iter->data == group.computeConfig) {
        MLC_S_DPRINTF(sendConfig->dynamicId,
                      "[PUMErased] Erase ForwardEdge -> %s.\n",
                      iter->data->dynamicId);
        deps.erase(iter);
        erased = true;
        break;
      }
    }
    assert(erased && "Failed to remove FowardEdge.");
    if (deps.empty()) {
      MLC_S_DPRINTF(sendConfig->dynamicId,
                    "[PUMErased] Erase Empty ForwardStream.\n");
      eraseFromNormalConfigs(sendConfig);
    }
  }
}

void MLCPUMManager::addPUMReduceStream(PUMContext &context,
                                       CacheStreamConfigureVec *configs,
                                       PUMComputeStreamGroup &group) {
  if (!group.appliedPUM) {
    return;
  }
  if (!group.reduceConfig) {
    return;
  }

  /**
   * We will use a special ReduceStream to collect partial result from PUM
   * reduction. Specifically, we will copy the DirectStream and ReductionStream
   * configuration, with the following changes:
   *
   * 1. The pattern is expanded to align with tile boundaries, and try to reduce
   * M elements with in that tile in that dimension. For example, if we have:
   *  - a 2D array of size MxN
   *  - tile size TmxTn
   *  - reduce dimension 0 (column)
   *  - reduce the sub-region [R1, R2) x [C1, C2)
   *  - PUM will produce P partial results in each tile.
   *
   * First we align the sub-region to tile boundary:
   *   TR1 = (R1 / Tm) * Tm, TR2 = ((R2 + Tm - 1) / Tm) * Tm
   *   TC1 = (C1 / Tn) * Tn, TC2 = ((C2 + Tn - 1) / Tn) * Tn
   *
   * Then we try to reduce P results from each tile:
   *   TR1*N+TC1 : 1 : P : Tn : (TC2-TC1)/Tn : N : TR2-TR1
   *
   * The second dimension is when we get the final reduction
   *
   * 2. We will have to properly change the edges between streams.
   *  - The NewReduceConfig and NewDirectConfig should point to each other,
   *  - NewReduceConfig should only use NewDirectConfig.
   *  - Any user of NewReduceConfig is kept the same.
   *
   * 3. We should really split the compuation into Reduce and Non-Reduce part,
   * and let PUM handle all Non-Reduce part while partial Reduce. Then here we
   * should change the computation of NewReduceConfig to only do Reduce part.
   *
   * However, right now we don;t have support to split the computation in the
   * compiler, thus here we replace it with an empty function.
   */

  const auto &directConfig = group.computeConfig;
  const auto &reduceConfig = group.reduceConfig;

  /**
   * @brief Make a copy of both configurations, and clear the edges.
   */
  auto newDirectConfig =
      std::make_shared<CacheStreamConfigureData>(*directConfig);
  auto newReduceConfig =
      std::make_shared<CacheStreamConfigureData>(*reduceConfig);
  newDirectConfig->clearEdges();
  newReduceConfig->clearEdges();

  const auto &patInfo = group.patternInfo.at(directConfig->stream);

  auto tileAndArraySize = patInfo.pumTile.getTileAndArraySize();
  auto tileSizes = tileAndArraySize.first;
  auto arraySizes = tileAndArraySize.second;

  const auto &atomicPat = patInfo.getSingleAtomicPat();
  assert(atomicPat.isSubRegionToArraySize(arraySizes));

  auto startPos = atomicPat.getSubRegionStartToArraySize(arraySizes);
  auto trips = atomicPat.getTrips();

  /**
   * @brief Align ONLY the reduced dimension to the tile boundary.
   */
  AffinePattern::IntVecT tileAlignedStartPos = startPos;
  AffinePattern::IntVecT tileAlignedTrips = trips;
  const int reducedDim = 0;
  {
    auto p = startPos.at(reducedDim);
    auto t = tileSizes.at(reducedDim);
    auto s = trips.at(reducedDim);
    auto q = p + s;

    auto pTile = (p / t) * t;
    auto qTile = ((q + t - 1) / t) * t;

    tileAlignedStartPos.at(reducedDim) = pTile;
    tileAlignedTrips.at(reducedDim) = qTile - pTile;
  }

  // Construct the pattern.
  auto tileAlignedAtomicPat = AffinePattern::constructSubRegion(
      arraySizes, tileAlignedStartPos, tileAlignedTrips);

  // Split the pattern at the first dimension to accumuate P partial results
  // from each tile. So far we assume we have (P = 1) partial result per tile.
  const int64_t partialResultsPerTile = 1;
  {

    auto paramSplitDimIter = tileAlignedAtomicPat.params.begin() + reducedDim;
    paramSplitDimIter->stride = tileSizes.at(reducedDim);
    paramSplitDimIter->trip /= tileSizes.at(reducedDim);

    tileAlignedAtomicPat.params.insert(
        paramSplitDimIter, AffinePattern::Param(1, partialResultsPerTile));
  }

  MLC_S_DPRINTF(directConfig->dynamicId,
                "[PUMReduce] TilePat %s ReducePat %s AlignedToTile %s.\n",
                patInfo.pumTile, atomicPat, tileAlignedAtomicPat);

  /**
   * @brief We needed to add back splitted outer dimension.
   * Also, we need to set the information to coordinate PUMReduceStream and
   * PUMEngine, and notify MLCStrandManager that streams should never be split
   * at these outer dimensions.
   */
  newDirectConfig->pumContextId = context.contextId;
  newDirectConfig->pumElemPerSync = tileAlignedAtomicPat.getTotalTrip();
  newDirectConfig->waitPUMRoundStart = false; // By default wait on Complete.
  newDirectConfig->hintNoStrandSplitOuterTripCount = 1;
  if (!patInfo.splitOuterDims.empty()) {
    const auto &splitDims = patInfo.splitOuterDims.front();
    tileAlignedAtomicPat.params.insert(tileAlignedAtomicPat.params.end(),
                                       splitDims.params.begin(),
                                       splitDims.params.end());
    newDirectConfig->hintNoStrandSplitOuterTripCount = splitDims.getTotalTrip();
    MLC_S_DPRINTF(directConfig->dynamicId,
                  "[PUMReduce] TileAlignedPat Added SplitOuterDim %s -> %s "
                  "NoStrandSplitOuterTripCount %ld.\n",
                  splitDims, tileAlignedAtomicPat,
                  newDirectConfig->hintNoStrandSplitOuterTripCount);
  }

  auto addrGenFormalParams = this->convertAffinePatternToStreamFormalParams(
      tileAlignedAtomicPat, patInfo.regionVAddr, patInfo.scalarElemSize);

  MLC_S_DPRINTF(directConfig->dynamicId,
                "[PUMReduce] Convert to AddrPattern RegionVAddr %#x "
                "ScalarElemSize %d %s.\n",
                patInfo.regionVAddr, patInfo.scalarElemSize,
                printAffinePatternParams(addrGenFormalParams));

  newDirectConfig->addrGenFormalParams = addrGenFormalParams;

  /**
   * Do not forget to adjust the TripCount.
   */
  auto newInnerTripCount = tileAlignedTrips.at(reducedDim) /
                           tileSizes.at(reducedDim) * partialResultsPerTile;
  newDirectConfig->totalTripCount = tileAlignedAtomicPat.getTotalTrip();
  newDirectConfig->innerTripCount = newInnerTripCount;
  newReduceConfig->totalTripCount = tileAlignedAtomicPat.getTotalTrip();
  newReduceConfig->innerTripCount = newInnerTripCount;

  MLC_S_DPRINTF(directConfig->dynamicId,
                "[PUMReduce]   NewTotalTrip %ld NewInnerTrip %ld.\n",
                newDirectConfig->totalTripCount,
                newDirectConfig->innerTripCount);

  /**
   * Normally we should split the reduction computation in the compiler.
   * For now as an approximation for the performance, we override the default
   * compute latency based on the last instruction.
   */
  newReduceConfig->overrideComputeLatency =
      newReduceConfig->stream->getComputeCallback()->getLastInstLat();

  /**
   * 2. Adjust the edges of our new configurations.
   */
  newDirectConfig->addUsedBy(newReduceConfig);

  /**
   * Copy any dependence on the ReduceConfig. Be careful, here we will check
   * that reuse is 1 and skip is the original InnerTripCount. And we will change
   * the skip to our new InnerTripCount.
   */
  for (const auto &dep : reduceConfig->depEdges) {
    assert(dep.type == CacheStreamConfigureData::DepEdge::Type::SendTo);
    auto recvConfig = dep.data;
    assert(recvConfig);
    assert(dep.reuse == 1);
    assert(dep.skip == directConfig->innerTripCount);

    auto newSkip = newReduceConfig->innerTripCount;
    newReduceConfig->addSendTo(recvConfig, dep.reuse, newSkip);

    // Also replace the edge in RecvConfig.
    bool replacedBaseEdge = false;
    for (auto &base : recvConfig->baseEdges) {
      if (base.dynStreamId == reduceConfig->dynamicId) {
        base.data = newReduceConfig;
        base.skip = newSkip;
        replacedBaseEdge = true;
        break;
      }
    }
    assert(replacedBaseEdge && "Failed to replace BaseEdge.");
  }

  // Copy the new ReduceStream.
  group.pumDirectConfig = newDirectConfig;
  group.pumReduceConfig = newReduceConfig;

  // ! Hack: Also make the ReduceFormalParams all invariant with value 1.
  for (auto &reduceFormalParam : newReduceConfig->addrGenFormalParams) {
    if (reduceFormalParam.isInvariant) {
      continue;
    }
    reduceFormalParam.isInvariant = true;
    reduceFormalParam.invariant.uint64() = 1;
  }

  /**
   * Insert back the new reduce configurations. Also remove it from
   * PurePUMConfigs.
   */
  configs->push_back(newDirectConfig);

  bool erasedPurePUMId = false;
  for (auto iter = context.purePUMStreamIds.begin();
       iter != context.purePUMStreamIds.end(); ++iter) {
    if ((*iter) == directConfig->dynamicId) {
      context.purePUMStreamIds.erase(iter);
      erasedPurePUMId = true;
      break;
    }
  }
  assert(erasedPurePUMId);
}

void MLCPUMManager::addPUMLoadStream(PUMContext &context,
                                     CacheStreamConfigureVec *configs,
                                     PUMDataGraphNode *loadNode) {
  /**
   * We will use a special LaodStream to broadcast data to PUM transposed
   * format. Specifically, we will copy the original LoadConfig and modify it's
   * SendTo edge to a special PUMSendTo edge.
   *
   * 1. The pattern is expanded to align with tile boundaries, and try to reduce
   * M elements with in that tile in that dimension. For example, if we have:
   *  - a 2D array of size MxN
   *  - tile size TmxTn
   *  - reduce dimension 0 (column)
   *  - reduce the sub-region [R1, R2) x [C1, C2)
   *  - PUM will produce P partial results in each tile.
   *
   * First we align the sub-region to tile boundary:
   *   TR1 = (R1 / Tm) * Tm, TR2 = ((R2 + Tm - 1) / Tm) * Tm
   *   TC1 = (C1 / Tn) * Tn, TC2 = ((C2 + Tn - 1) / Tn) * Tn
   *
   * Then we try to reduce P results from each tile:
   *   TR1*N+TC1 : 1 : P : Tn : (TC2-TC1)/Tn : N : TR2-TR1
   *
   * The second dimension is when we get the final reduction
   *
   * 2. We will have to properly change the edges between streams.
   *  - The NewReduceConfig and NewDirectConfig should point to each other,
   *  - NewReduceConfig should only use NewDirectConfig.
   *  - Any user of NewReduceConfig is kept the same.
   *
   * 3. We should really split the compuation into Reduce and Non-Reduce part,
   * and let PUM handle all Non-Reduce part while partial Reduce. Then here we
   * should change the computation of NewReduceConfig to only do Reduce part.
   *
   * However, right now we don;t have support to split the computation in the
   * compiler, thus here we replace it with an empty function.
   */

  const auto &sendConfig = loadNode->sendConfig;
  const auto &sendDynId = sendConfig->dynamicId;
  const auto &sendPat = loadNode->sendPat;

  const auto &recvConfig = loadNode->recvConfig;
  const auto &recvPat = loadNode->pattern;
  const auto &recvSplitOutDim = loadNode->splitOutDim;

  MLC_S_DPRINTF(sendDynId,
                "[PUMLoad] ---- SendPat %s RecvPat %s RecvSplitOutDim %s.\n",
                sendPat, recvPat, recvSplitOutDim);

  auto sendLoopLevel = sendConfig->stream->getLoopLevel();
  auto recvLoopLevel = recvConfig->stream->getLoopLevel();

  if (sendLoopLevel > recvLoopLevel) {
    MLC_S_PANIC_NO_DUMP(sendDynId,
                        "[PUMLoad] Can not handle inner-to-outer PUMLoad.");
  }

  auto loopLevelDiff = recvLoopLevel - sendLoopLevel;
  if (recvPat.numDimension() < loopLevelDiff) {
    MLC_S_PANIC_NO_DUMP(
        sendDynId,
        "[PUMLoad] Can not handle reused PUMLoad across SplitOutDim.");
  }

  /**
   * As a heuristic, if the recv pattern is large, expand it to the whole array
   * so that we can get simple masks.
   * TODO: Implement this.
   */

  /**
   * The first step is to reorganize the recv patterns:
   * 1. Dimension [0, LoopLevelDiff) is considered BroadcastPattern.
   * 2. Dimension [LoopLevelDiff, OutMost), is considered now SplitOutDim.
   * 3. The original SplitOutDim is considered now Wait for round.
   */
  auto broadcastPat = recvPat;
  auto newRecvPat = broadcastPat.splitFromDim(loopLevelDiff);
  newRecvPat.mergeOutDim(recvSplitOutDim);

  // Fill in missing dimensions of BroadcastPattern.
  auto arraySizes = loadNode->pumTile.getArraySize();
  int64_t accArraySize = 1;
  for (int dim = 0; dim < arraySizes.size(); ++dim) {
    if (broadcastPat.params.size() <= dim) {
      // Add back one dimention with the correct stride and trip equals 1.
      broadcastPat.params.push_back(AffinePattern::Param(accArraySize, 1));
    }
    accArraySize *= arraySizes[dim];
  }

  MLC_S_DPRINTF(sendDynId, "[PUMLoad]   BroadcastPat %s NewRecvPat %s.\n",
                broadcastPat, newRecvPat);

  /**
   * Make a copy of both configurations, and clear the edges.
   */
  auto newSendConfig = std::make_shared<CacheStreamConfigureData>(*sendConfig);
  newSendConfig->clearEdges();

  // Set the PUMSendToEdge.
  newSendConfig->addPUMSendTo(recvConfig, broadcastPat, newRecvPat,
                              loadNode->pumTile);

  // PUMSendTo Streams can not be sliced.
  newSendConfig->shouldBeSlicedToCacheLines = false;

  /**
   * We set the information to coordinate the PUMLoadStream and PUMEngine.
   * TODO: Correctly set the Sync count as now we have multiple syncs per
   * TODO: compute round.
   */
  auto pumElemPerSync = sendPat.getTotalTrip();
  if (recvSplitOutDim.getTotalTrip() != 0) {
    pumElemPerSync = sendPat.getTotalTrip() / recvSplitOutDim.getTotalTrip();
    MLC_S_DPRINTF(sendDynId, "[PUMLoad]   PUMElemPerSync = %ld / %ld = %ld.\n",
                  sendPat.getTotalTrip(), recvSplitOutDim.getTotalTrip(),
                  pumElemPerSync);
  } else {
    MLC_S_DPRINTF(sendDynId, "[PUMLoad]   PUMElemPerSync = %ld.\n",
                  pumElemPerSync);
  }
  newSendConfig->pumContextId = context.contextId;
  newSendConfig->pumElemPerSync = pumElemPerSync;
  newSendConfig->waitPUMRoundStart = true;

  /**
   * If we have SplitOutDim, notify MLCStrandManager that streams should not be
   * splitted at these outer-dimensions.
   */
  newSendConfig->hintNoStrandSplitOuterTripCount = 1;
  if (recvSplitOutDim.getTotalTrip() != 0) {
    newSendConfig->hintNoStrandSplitOuterTripCount =
        recvSplitOutDim.getTotalTrip();
  }

  /**
   * Insert back the new reduce configurations. Also remove it from
   * PurePUMConfigs.
   */
  configs->push_back(newSendConfig);

  bool erasedPurePUMId = false;
  for (auto iter = context.purePUMStreamIds.begin();
       iter != context.purePUMStreamIds.end(); ++iter) {
    if ((*iter) == sendDynId) {
      context.purePUMStreamIds.erase(iter);
      erasedPurePUMId = true;
      break;
    }
  }
  assert(erasedPurePUMId);
}

void MLCPUMManager::receiveStreamConfigure(PacketPtr pkt) {

  if (this->controller->myParams->stream_pum_mode != 1) {
    this->runMLCConfigWithoutPUM(pkt);
    return;
  }

  this->setPUMManagerAtPUMEngine();

  this->contexts.emplace_back();
  auto &context = this->contexts.back();

  // Take a copy of the configure vector.
  context.configs = **(pkt->getPtr<CacheStreamConfigureVec *>());
  this->findPUMComputeStreamGroups(context);

  for (auto &group : context.pumGroups) {
    group.canApplyPUM = this->canApplyPUMToGroup(context, group);
  }

  bool appliedPUM = false;
  for (auto &group : context.pumGroups) {
    if (group.canApplyPUM) {
      /**
       * For now we always apply PUM if we can.
       * Increment the numFloatPUM stats.
       */
      group.appliedPUM = true;
      appliedPUM = true;
      group.computeConfig->stream->statistic.numFloatPUM++;
    }
  }

  if (!appliedPUM) {
    this->contexts.pop_back();
    this->runMLCConfigWithoutPUM(pkt);
    return;
  }

  // Record the initialize cycle.
  context.initCycle = this->controller->curCycle();

  // If this is the first context, record the cycle.
  if (this->contexts.size() == 1) {
    this->firstContextInitCycle = this->controller->curCycle();
  }

  /**
   * Clear the PUMConfigs from the original ConfigVec so that MLCStreamEngine
   * can continue to handle normal streams.
   */
  auto normalConfigs = *(pkt->getPtr<CacheStreamConfigureVec *>());
  for (auto &group : context.pumGroups) {
    this->erasePUMConfigs(context, normalConfigs, group);
  }
  for (auto &group : context.pumGroups) {
    this->addPUMReduceStream(context, normalConfigs, group);
  }

  // Build the PUMDataGraph.
  this->buildPUMDataGraph(context);

  // LoadNode is preoffloaded as special PUMLoadStream.
  for (auto node : context.pumDataGraphNodes) {
    if (node->type == PUMDataGraphNode::TypeE::Load) {
      this->addPUMLoadStream(context, normalConfigs, node);
    }
  }

  CacheStreamConfigureVec *prefetchConfigs = new CacheStreamConfigureVec;
  for (auto &group : context.pumGroups) {
    if (group.appliedPUM) {
      // Prefetch necessary data.
      auto pConfigs = this->generatePrefetchStreams(group);
      prefetchConfigs->insert(prefetchConfigs->end(), pConfigs.begin(),
                              pConfigs.end());
    }
  }
  // Remember the prefetch configs.
  context.prefetchConfigs = *prefetchConfigs;
  MLCSE_DPRINTF("%d prefetch streams generated.\n", prefetchConfigs->size());

  assert(!context.savedPkt);
  context.savedPkt = pkt;

  if (prefetchConfigs->empty()) {
    this->runPUMExecutionStage(context);
  } else {
    this->runPrefetchStage(context, prefetchConfigs);
  }

  return;
}

void MLCPUMManager::postMLCSEConfigure(PUMContext &context) {

  assert(context.waitingPostConfig);
  context.waitingPostConfig = false;

  this->compileContext(context);
  if (this->contexts.front().contextId == context.contextId) {
    // Start PUM only if we reached the front of the queue.
    this->configurePUMEngine(context);
  }
}

void MLCPUMManager::setPUMManagerAtPUMEngine() {
  for (int row = 0; row < this->controller->getNumRows(); ++row) {
    for (int col = 0; col < this->controller->getNumCols(); ++col) {
      int nodeId = row * this->controller->getNumCols() + col;
      MachineID dstMachineId(MachineType_L2Cache, nodeId);

      /**
       * We still configure here. But PUMEngine will not start until received
       * the Kick.
       */
      auto llcCntrl =
          AbstractStreamAwareController::getController(dstMachineId);
      auto llcSE = llcCntrl->getLLCStreamEngine();
      llcSE->getPUMEngine()->setPUMManager(this);
    }
  }
}

void MLCPUMManager::configurePUMEngine(PUMContext &context) {
  for (int row = 0; row < this->controller->getNumRows(); ++row) {
    for (int col = 0; col < this->controller->getNumCols(); ++col) {
      int nodeId = row * this->controller->getNumCols() + col;
      MachineID dstMachineId(MachineType_L2Cache, nodeId);

      /**
       * We still configure here. But PUMEngine will not start until received
       * the Kick.
       */
      auto llcCntrl =
          AbstractStreamAwareController::getController(dstMachineId);
      auto llcSE = llcCntrl->getLLCStreamEngine();
      llcSE->getPUMEngine()->configure(this, context.contextId,
                                       context.commands);
    }
  }
  assert(context.state == PUMContext::StateE::Initialized);
  /**
   * Accumulate the latency to compile the kernel template.
   * So far we charge a fixed 100 cycles latency for non-sync commands.
   * Then we schedule the event of the MLC SE to kick this context.
   */
  if (context.waitingFirstCompileDone) {
    assert(context.firstCompileReadyCycle == Cycles(0));

    int totalCompileLatency = 0;
    const int compileLatencyPerCommand =
        this->controller->myParams->stream_pum_compile_lat_per_cmd;
    for (const auto &command : context.commands) {
      if (command.type == "sync") {
        continue;
      }
      totalCompileLatency += compileLatencyPerCommand;
    }
    this->controller->m_statPUMCompileCycles += totalCompileLatency;
    context.firstCompileReadyCycle =
        this->controller->curCycle() + Cycles(totalCompileLatency);

    auto contextId = context.contextId;
    auto event = new EventFunctionWrapper(
        [this, contextId]() -> void {
          this->kickPUMEngineEventImpl(contextId);
        },
        "MLCPUMManager::kick", true /* del */
    );

    this->controller->schedule(
        event, this->controller->cyclesToTicks(context.firstCompileReadyCycle));

    return;
  }

  // This is not the first time compiling. Assuming pipelined compilation,
  // we do not charge compilation latency any more.
  context.state = PUMContext::StateE::Kicked;
  context.lastKickCycle = this->controller->curCycle();
  this->kickPUMEngine(context, MessageSizeType_Data, false /* isIdea */);
}

void MLCPUMManager::kickPUMEngineEventImpl(int64_t contextId) {
  for (auto &context : this->contexts) {
    if (context.contextId != contextId) {
      continue;
    }
    // Found the context.
    assert(context.waitingFirstCompileDone);
    assert(context.state == PUMContext::StateE::Initialized);
    assert(context.firstCompileReadyCycle <= this->controller->curCycle());
    context.waitingFirstCompileDone = false;
    context.state = PUMContext::StateE::Kicked;
    context.lastKickCycle = this->controller->curCycle();
    this->kickPUMEngine(context, MessageSizeType_Data, false /* isIdea */);
    return;
  }
  // Sliently ignore the case when we do not find the context.
}

void MLCPUMManager::kickPUMEngine(PUMContext &context, MessageSizeType sizeType,
                                  bool isIdea) {

  context.lastSyncCycle = this->controller->curCycle();

  /**
   * Broadcast the kick packet.
   * So far this is implemented as a PUMConfig packet.
   */
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = 0;
  msg->m_Type = CoherenceRequestType_STREAM_CONFIG;
  msg->m_Requestors.add(this->controller->getMachineID());
  msg->m_MessageSize = sizeType;
  msg->m_isPUM = true;

  if (isIdea) {
    for (int row = 0; row < this->controller->getNumRows(); ++row) {
      for (int col = 0; col < this->controller->getNumCols(); ++col) {
        int nodeId = row * this->controller->getNumCols() + col;
        MachineID dstMachineId(MachineType_L2Cache, nodeId);
        msg->m_Destination.add(dstMachineId);

        /**
         * We still configure here. But PUMEngine will not start until
         * received the configuration message.
         */
        auto llcCntrl =
            AbstractStreamAwareController::getController(dstMachineId);
        auto llcSE = llcCntrl->getLLCStreamEngine();
        llcSE->getPUMEngine()->receiveKick(*msg);

        msg->m_Destination.clear();
      }
    }
    return;
  }

  for (int row = 0; row < this->controller->getNumRows(); ++row) {
    for (int col = 0; col < this->controller->getNumCols(); ++col) {
      int nodeId = row * this->controller->getNumCols() + col;
      MachineID dstMachineId(MachineType_L2Cache, nodeId);

      msg->m_Destination.add(dstMachineId);
    }
  }

  Cycles latency(1); // Just use 1 cycle latency here.

  MLCSE_DPRINTF("Broadcast PUMKick.\n");

  mlcSE->requestToLLCMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(latency));
}

AffinePatternVecT MLCPUMManager::decoalesceAndDevectorizePattern(
    const ConfigPtr &config, const AffinePattern &pattern, int scalarElemSize) {
  AffinePatternVecT ret;

  for (const auto &id : config->stream->getLogicalStreamIds()) {
    int32_t offset = 0;
    int32_t size = 0;
    config->stream->getCoalescedOffsetAndSize(id, offset, size);
    AssertScalarAlign(offset);
    AssertScalarAlign(size);

    AffinePattern newPat(pattern);

    // Decoalesce.
    newPat.start += AlignToScalarElem(offset);

    // Devectorize.
    if (size > scalarElemSize) {
      assert(newPat.params.size() >= 1);
      auto stride0 = newPat.params.at(0).stride;
      auto scalarSize = AlignToScalarElem(size);
      if (stride0 == scalarSize) {
        // A heuristic that the first is vectorized.
        newPat.params.at(0).stride = 1;
        newPat.params.at(0).trip *= scalarSize;
      } else {
        // Add a new dimension.
        AffinePattern::Param param(1, scalarSize);
        newPat.params.insert(newPat.params.begin(), param);
      }
    }

    ret.push_back(newPat);
  }

  return ret;
}

DynStreamFormalParamV MLCPUMManager::convertAffinePatternToStreamFormalParams(
    const AffinePattern &pattern, Addr arrayVAddr, int64_t memElemSize) const {

  DynStreamFormalParamV params;

  uint64_t prevTrip = 1;
  for (const auto &param : pattern.params) {
    auto stride = param.stride;
    auto trip = param.trip;
    auto memStride = stride * memElemSize;
    params.emplace_back();
    params.back().isInvariant = true;
    params.back().invariant.uint64() = memStride;

    params.emplace_back();
    params.back().isInvariant = true;
    params.back().invariant.uint64() = trip * prevTrip;
    prevTrip = trip * prevTrip;
  }

  auto startVAddr = pattern.start * memElemSize + arrayVAddr;
  params.emplace_back();
  params.back().isInvariant = true;
  params.back().invariant.uint64() = startVAddr;

  return params;
}

void MLCPUMManager::preprocessPatternsInGroup(PUMContext &context,
                                              PUMComputeStreamGroup &group) {

  const auto &groupDynId = group.computeConfig->dynamicId;

  auto recvConfig = group.computeConfig;
  auto recvS = recvConfig->stream;
  auto &recvPatInfo = group.patternInfo.at(recvS);
  auto recvTile = recvPatInfo.pumTile;

  MLC_S_DPRINTF(groupDynId, "[PUM] Preprocess Patterns in Group.\n");

  for (const auto &sendConfig : group.usedPUMConfigs) {

    const auto &sendDynId = sendConfig->dynamicId;
    auto S = sendConfig->stream;
    auto &sendPatInfo = group.patternInfo.at(S);

    for (auto &sendPat : sendPatInfo.atomicPatterns) {

      auto reusedSendPat =
          this->addReuseToOuterPattern(sendConfig, recvConfig, sendPat);
      MLC_S_DPRINTF(sendDynId, "[PUM]   AddReuse SendPattern %s -> %s.\n",
                    sendPat, reusedSendPat);

      sendPat = reusedSendPat;
    }
  }

  /**
   * If we have more dimension than the array, we try to split out the
   * OuterLoop and handle that sequentially.
   *
   * So far we only support split one more dimension, and require all patterns
   * has the same dimension.
   */
  auto &recvPat = recvPatInfo.atomicPatterns.front();
  auto arrayDims = recvTile.params.size() / 2;
  if (recvPat.params.size() != arrayDims + 1) {
    return;
  }

  bool shouldTrySplitOuterDim = true;
  MLC_S_DPRINTF(groupDynId, "[PUM]   Check ShouldTrySplitOuterDim.\n");
  for (const auto &sendConfig : group.usedPUMConfigs) {

    const auto &sendDynId = sendConfig->dynamicId;
    auto S = sendConfig->stream;
    auto &sendPatInfo = group.patternInfo.at(S);

    for (auto &sendPat : sendPatInfo.atomicPatterns) {

      if (sendPat.params.size() != recvPat.params.size()) {
        MLC_S_DPRINTF(sendDynId, "[PUM]     SendPat %s Mismatch in Dim.\n",
                      sendPat);
        shouldTrySplitOuterDim = false;
        break;
      }

      if (sendPat.params.back().trip != recvPat.params.back().trip) {
        MLC_S_DPRINTF(sendDynId, "[PUM]     SendPat %s Mismatch in Trip.\n",
                      sendPat);
        shouldTrySplitOuterDim = false;
        break;
      }
    }

    if (!shouldTrySplitOuterDim) {
      break;
    }
  }
  if (!shouldTrySplitOuterDim) {
    return;
  }

  // Try to split the outer dimension.
  group.outerDimSplitted = true;
  for (const auto &sendConfig : group.usedPUMConfigs) {

    const auto &sendDynId = sendConfig->dynamicId;
    auto S = sendConfig->stream;
    auto &sendPatInfo = group.patternInfo.at(S);
    for (auto &sendPat : sendPatInfo.atomicPatterns) {

      auto splitPat = sendPat.splitFromDim(arrayDims);
      MLC_S_DPRINTF(sendDynId, "[PUM]     SendPat Split into %s %s.\n", sendPat,
                    splitPat);

      sendPatInfo.splitOuterDims.push_back(splitPat);
    }
  }

  for (auto &recvPat : recvPatInfo.atomicPatterns) {
    auto recvSplitPat = recvPat.splitFromDim(arrayDims);
    MLC_S_DPRINTF(groupDynId, "[PUM]     RecvPat Split into %s %s.\n", recvPat,
                  recvSplitPat);

    recvPatInfo.splitOuterDims.push_back(recvSplitPat);
  }
}

AffinePattern
MLCPUMManager::addReuseToOuterPattern(const ConfigPtr &outerConfig,
                                      const ConfigPtr &innerConfig,
                                      const AffinePattern &pattern) const {

  auto outerS = outerConfig->stream;
  auto innerS = innerConfig->stream;
  assert(outerS->getLoopLevel() <= innerS->getLoopLevel() &&
         "Outer should be outside.");
  if (outerS->getLoopLevel() == innerS->getLoopLevel()) {
    // Nothing to do.
    return pattern;
  }

  assert(outerConfig->hasTotalTripCount());
  assert(innerConfig->hasTotalTripCount());
  auto outerTripCount = outerConfig->getTotalTripCount();
  auto innerTripCount = innerConfig->getTotalTripCount();
  assert(innerTripCount % outerTripCount == 0);
  auto reuseCount = innerTripCount / outerTripCount;

  /**
   * TODO: Handle when LoopLevel difference is greater than 1.
   */
  auto start = pattern.start;
  auto params = pattern.params;
  params.insert(params.begin(), AffinePattern::Param(0, reuseCount));
  auto reusedPattern = AffinePattern(start, params);
  MLC_S_DPRINTF(outerConfig->dynamicId, "[PUM] AddReuse %ld -> %s.\n",
                reuseCount, reusedPattern);
  return reusedPattern;
}

MLCPUMManager::PUMContext &MLCPUMManager::getFirstKickedContext() {
  assert(!this->contexts.empty() && "No PUMContext.");
  for (auto &context : this->contexts) {
    if (context.state == PUMContext::StateE::Kicked) {
      return context;
    }
  }
  panic("No KickedContext.");
}

MLCPUMManager::PUMContext *MLCPUMManager::getFirstInitializedContext() {
  if (this->contexts.empty()) {
    return nullptr;
  }
  for (auto &context : this->contexts) {
    if (context.state == PUMContext::StateE::Initialized) {
      return &context;
    }
  }
  return nullptr;
}

MLCPUMManager::PUMContext &MLCPUMManager::getContextById(int64_t contextId) {
  for (auto &context : this->contexts) {
    if (context.contextId == contextId) {
      return context;
    }
  }
  panic("No context with id %ld.", contextId);
}

void MLCPUMManager::reachSync(int sentPackets) {

  auto &context = this->getFirstKickedContext();
  assert(context.isActive() && "No Active PUM.");
  context.receivedAcks++;
  context.totalSentPackets += sentPackets;
  assert(context.reachedSync < context.totalSyncs && "Sync overflow.");
  MLCSE_DPRINTF(
      "MLC ReachedSync %d Recv Ack %d ExpectedAck %d TotalSentPkt + %d = %d.\n",
      context.reachedSync, context.receivedAcks,
      context.expectedAcksEverySync.at(context.reachedSync), sentPackets,
      context.totalSentPackets);
  this->checkSync(context);
}

void MLCPUMManager::receivePacket(int recvPackets) {
  auto &context = this->getFirstKickedContext();
  assert(context.isActive() && "No Active PUM.");
  context.totalRecvPackets += recvPackets;
  MLCSE_DPRINTF("MLC TotalRecvPkt + %d = %d.\n", recvPackets,
                context.totalRecvPackets);
  this->checkSync(context);
}

void MLCPUMManager::checkSync(PUMContext &context) {

  assert(context.isActive() && "No Active PUM.");

  if (context.reachedSync == context.totalSyncs) {
    return;
  }

  auto expectedAcks = context.expectedAcksEverySync.at(context.reachedSync);
  if (context.receivedAcks == expectedAcks &&
      context.totalSentPackets == context.totalRecvPackets) {

    MLCSE_DPRINTF("Synced %d Total %d.\n", context.reachedSync,
                  context.totalSyncs);
    context.reachedSync++;
    context.receivedAcks = 0;
    context.totalSentPackets = 0;
    context.totalRecvPackets = 0;

    auto cyclesBetweenSync =
        this->controller->curCycle() - context.lastSyncCycle;
    for (const auto &group : context.pumGroups) {
      if (group.appliedPUM) {
        group.computeConfig->stream->statistic.samplePUMCyclesBetweenSync(
            cyclesBetweenSync, context.reachedSync - 1);
      }
    }

    // We check if the current expected sync is more than number of LLC banks.
    // If so, we record this cycles as MixCycles.
    if (context.expectedAcksEverySync.at(context.reachedSync - 1) >
        this->controller->getNumCols() * this->controller->getNumRows()) {
      this->controller->m_statPUMMixCycles += cyclesBetweenSync;
    }

    if (context.reachedSync == context.totalSyncs) {
      // This is the last Sync.
      this->completeOneComputeRound(context);
    } else {
      // Notify the PUMEngine to continue.
      this->kickPUMEngine(
          context, MessageSizeType_Control,
          this->controller->myParams->enable_stream_idea_ack /* isIdea */);
    }
  }
}

void MLCPUMManager::reportProgress(int64_t contextId) {
  const auto &context = this->getContextById(contextId);
  for (const auto &group : context.pumGroups) {
    if (!group.appliedPUM) {
      continue;
    }
    // Record that we have made some progress.
    const auto &config = group.computeConfig;
    auto S = config->stream;
    S->incrementOffloadedStepped();
  }
}

void MLCPUMManager::completeOneComputeRound(PUMContext &context) {

  MLCSE_DPRINTF("[PUM] Complete One ComputeRound.\n");

  bool allGroupsDone = true;
  bool someGroupsDone = false;

  for (auto &group : context.pumGroups) {
    if (!group.appliedPUM) {
      continue;
    }

    const auto &config = group.computeConfig;
    auto S = config->stream;
    // Record that we have made some progress.
    S->incrementOffloadedStepped();
    auto dynS = S->getDynStream(config->dynamicId);
    if (!dynS) {
      MLC_S_PANIC_NO_DUMP(config->dynamicId, "No CoreDynS.");
    }
    if (dynS->shouldCoreSEIssue()) {
      MLC_S_PANIC_NO_DUMP(config->dynamicId,
                          "CoreSE should not issue for PUM.");
    }

    auto outerTripCount = 1;
    if (group.outerDimSplitted) {
      const auto &patInfo = group.patternInfo.at(S);
      assert(patInfo.splitOuterDims.size() >= 1);
      outerTripCount = patInfo.splitOuterDims.front().getTotalTrip();
      for (const auto &split : patInfo.splitOuterDims) {
        assert(split.getTotalTrip() == outerTripCount &&
               "Mismatch in Outer TotalTripCount.");
      }
    }

    if (S->isStoreComputeStream() || S->isAtomicComputeStream() ||
        S->isUpdateStream()) {
      // These are streams waiting for Ack.
      assert(config->hasTotalTripCount());
      auto tripCount = config->getTotalTripCount();

      // Be careful to only ack elements of the last compute round.
      auto innerTripCount = tripCount / outerTripCount;

      auto ackElemStart = innerTripCount * (group.nextOuterIter - 1);
      auto ackElemEnd = innerTripCount * group.nextOuterIter;
      MLC_S_DPRINTF(config->dynamicId, "[PUM] Ack Elem in [%ld, %ld).\n",
                    ackElemStart, ackElemEnd);

      for (int64_t elemIdx = ackElemStart; elemIdx < ackElemEnd; ++elemIdx) {
        dynS->cacheAckedElements.insert(elemIdx);
      }
    }

    if (group.nextOuterIter < outerTripCount) {
      MLC_S_DPRINTF(
          config->dynamicId,
          "[PUM] Not done yet NextOuterIter %ld OuterTripCount %ld.\n",
          group.nextOuterIter, outerTripCount);
      allGroupsDone = false;
    } else {
      someGroupsDone = true;
    }
  }

  if (allGroupsDone) {
    context.state = PUMContext::StateE::Done;
    return;
  }

  assert(!someGroupsDone && "Cannot support partially PUM done ><.");

  this->tryKickNextComputeRound(context);
}

void MLCPUMManager::tryKickNextComputeRound(PUMContext &context) {

  for (auto &group : context.pumGroups) {
    if (!group.appliedPUM || !group.reduceConfig || group.nextOuterIter == 0) {
      // This is not PUMReduction, or this is the first round.
      continue;
    }
    const auto &pumReduceConfig = group.pumReduceConfig;

    auto reducedElemsPerRound = group.pumDirectConfig->pumElemPerSync /
                                group.pumDirectConfig->innerTripCount;

    auto reducedElems = reducedElemsPerRound * group.nextOuterIter;
    // auto prevReducedElems = reducedElemsPerRound * (group.nextOuterIter - 1);
    assert(reducedElems > 0);
    auto ackedElemIdx = reducedElems - 1;

    for (const auto &dep : pumReduceConfig->depEdges) {
      const auto &depId = dep.data->dynamicId;
      assert(dep.data->stream->isStoreComputeStream());

      auto contextId = context.contextId;
      auto callback = [this, contextId](const DynStreamId &dynStreamId,
                                        uint64_t elemIdx) -> void {
        this->tryKickNextComputeRound(this->getContextById(contextId));
      };
      if (!this->mlcSE->strandManager->isStreamElemAcked(depId, ackedElemIdx,
                                                         callback)) {
        MLC_S_DPRINTF(
            group.computeConfig->dynamicId,
            "[PUMReduce] The CoreDynS %s No Ack for Round %ld Elem %lu.\n",
            depId, group.nextOuterIter, ackedElemIdx);
        if (context.lastBlockedByReduceCycle == Cycles(0)) {
          context.lastBlockedByReduceCycle = this->controller->curCycle();
        }
        return;
      }
    }
  }

  // We can start next round.
  context.clear();
  context.state = PUMContext::StateE::Initialized;

  // Record the BlockedByReduce as mixed cycles.
  if (context.lastBlockedByReduceCycle != Cycles(0)) {
    this->controller->m_statPUMReduceCycles +=
        this->controller->curCycle() - context.lastBlockedByReduceCycle;
    context.lastBlockedByReduceCycle = Cycles(0);
  }
  this->compileContext(context);
  this->configurePUMEngine(context);
}

void MLCPUMManager::receiveStreamEnd(std::vector<DynStreamId> &endIds) {

  /**
   * Search in contexts to find the matching one.
   * If not found, this means this configure is not handled as PUM.
   * Also, we only support releasing contexts in Done or Initialized state.
   *
   * TODO: Support releasing context in Kicked state.
   */

  auto iter = this->contexts.begin();
  while (iter != this->contexts.end()) {
    if (iter->configs.size() != endIds.size()) {
      continue;
    }
    bool matched = true;
    for (int i = 0; i < endIds.size(); ++i) {
      bool found = false;
      const auto &endId = endIds.at(i);
      for (int j = 0; j < iter->configs.size(); ++j) {
        if (iter->configs.at(j)->dynamicId == endId) {
          found = true;
          break;
        }
      }
      if (!found) {
        matched = false;
        break;
      }
    }

    if (matched) {
      break;
    }

    ++iter;
  }

  if (iter == this->contexts.end()) {
    return;
  }

  auto &context = *iter;
  if (context.state != PUMContext::StateE::Done &&
      context.state != PUMContext::StateE::Initialized) {
    for (const auto &endId : endIds) {
      MLC_S_DPRINTF(endId, "[PUM]   Received StreamEnd.\n");
    }
    for (const auto &group : context.pumGroups) {
      MLC_S_DPRINTF(group.computeConfig->dynamicId,
                    "[PUM]   Current in PUMContext. NextOutIter %ld.\n",
                    group.nextOuterIter);
    }
    MLC_S_PANIC_NO_DUMP(context.configs.front()->dynamicId,
                        "[PUM] Releasing but Not Done or Initialized.");
  }

  for (auto &dynId : context.purePUMStreamIds) {
    bool erased = false;
    for (auto iter = endIds.begin(); iter != endIds.end(); ++iter) {
      if (dynId == (*iter)) {
        endIds.erase(iter);
        erased = true;
        break;
      }
    }
    if (!erased) {
      MLC_S_PANIC_NO_DUMP(
          dynId, "[PUM] PurePUMStream not in EndIds. Are StreamEnds in order?");
    }
  }

  MLCSE_DPRINTF("Release PUM context.\n");
  bool isDone = (context.state == PUMContext::StateE::Done);

  this->contexts.erase(iter);

  // If this is the last context, record the total PUM cycles.
  if (this->contexts.empty()) {
    Cycles totalPUMCycles =
        this->controller->curCycle() - this->firstContextInitCycle;
    this->controller->m_statPUMTotalCycles += totalPUMCycles;
  }

  // Kick next one if found.
  if (isDone) {
    if (auto nextContext = this->getFirstInitializedContext()) {
      this->configurePUMEngine(*nextContext);
    }
  }
}

void MLCPUMManager::completeFinalReduction(PUMContext &context,
                                           PUMComputeStreamGroup &group) {

  auto reduceConfig = group.reduceConfig;
  assert(reduceConfig && "No ReduceConfig.");

  /**
   * Notify the final reduction value.
   * For now just set some fake value.
   */
  const auto &dynId = reduceConfig->dynamicId;
  auto S = reduceConfig->stream;
  auto dynCoreS = S->getDynStream(dynId);
  if (!dynCoreS) {
    MLC_S_PANIC_NO_DUMP(
        dynId, "[PUM] CoreDynS released before receiving FinalReductionValue.");
  }

  const auto &patInfo = group.patternInfo.at(group.computeConfig->stream);
  assert(patInfo.atomicPatterns.size() == 1);
  auto reducedTripCount = patInfo.atomicPatterns.front().getTotalTrip();

  assert(reduceConfig->hasInnerTripCount());
  auto innerTripCount = reduceConfig->getInnerTripCount();

  assert(reducedTripCount % innerTripCount == 0);
  assert(reducedTripCount >= innerTripCount);

  assert(group.nextOuterIter > 0);
  auto curOuterIter = group.nextOuterIter - 1;

  auto outerElemOffset = curOuterIter * reducedTripCount;

  for (uint64_t elemIdx = innerTripCount; elemIdx <= reducedTripCount;
       elemIdx += innerTripCount) {

    auto realElemIdx = elemIdx + outerElemOffset;

    // So far just some fake value.
    group.reductionResults.emplace_back(realElemIdx);

    MLC_S_DPRINTF(dynId,
                  "[PUM] Set ReductionResult OuterElemOffset %ld "
                  "InnerTripCount %ld ReducedTripCount %ld ElemIdx %ld.\n",
                  outerElemOffset, innerTripCount, reducedTripCount,
                  realElemIdx);
  }

  while (!group.reductionResults.empty()) {
    this->sendOneReductionResult(context, group);
  }
}

void MLCPUMManager::sendOneReductionResult(PUMContext &context,
                                           PUMComputeStreamGroup &group) {
  if (group.reductionResults.empty()) {
    return;
  }

  const auto &result = group.reductionResults.front();

  const auto &reduceConfig = group.reduceConfig;
  assert(reduceConfig && "No ReduceConfig.");
  const auto &dynId = reduceConfig->dynamicId;

  /**
   * Notify the final reduction value.
   * For now just set some fake value.
   */

  if (reduceConfig->finalValueNeededByCore) {
    auto S = reduceConfig->stream;
    auto dynCoreS = S->getDynStream(dynId);
    if (!dynCoreS) {
      MLC_S_PANIC_NO_DUMP(
          dynId,
          "[PUM] CoreDynS released before receiving FinalReductionValue.");
    }

    dynCoreS->setInnerFinalValue(result.elemIdx, result.value);
    MLC_S_DPRINTF(dynId,
                  "[PUM] SendBack ReductionResult ElemIdx %ld Value %s.\n",
                  result.elemIdx, result.value);
  }

  DynStreamSliceId sliceId;
  sliceId.vaddr = 0;
  sliceId.size = reduceConfig->elementSize;
  sliceId.getDynStrandId() =
      DynStrandId(reduceConfig->dynamicId, reduceConfig->strandIdx,
                  reduceConfig->totalStrands);
  sliceId.getStartIdx() = result.elemIdx;
  sliceId.getEndIdx() = result.elemIdx + 1;

  DataBlock dataBlock;
  dataBlock.setData(result.value.uint8Ptr(), 0, reduceConfig->elementSize);
  for (const auto &edge : reduceConfig->depEdges) {

    if (edge.type != CacheStreamConfigureData::DepEdge::Type::SendTo) {
      continue;
    }

    const auto &recvConfig = edge.data;
    auto recvElemIdx = CacheStreamConfigureData::convertBaseToDepElemIdx(
        result.elemIdx, edge.reuse, edge.skip);

    MLC_S_DPRINTF(dynId,
                  "[PUM] Send ReductionResult ElemIdx %ld Value %s To %s "
                  "RecvElem %lu.\n",
                  result.elemIdx, result.value, recvConfig->dynamicId,
                  recvElemIdx);
    this->mlcSE->issueStreamDataToLLC(sliceId, dataBlock, recvConfig,
                                      recvElemIdx, reduceConfig->elementSize);
  }

  group.reductionResults.pop_front();
}
