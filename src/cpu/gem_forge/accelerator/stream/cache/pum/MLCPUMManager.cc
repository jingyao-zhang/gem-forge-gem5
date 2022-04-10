#include "MLCPUMManager.hh"
#include "PUMEngine.hh"

#include "../LLCStreamEngine.hh"
#include "../MLCStrandManager.hh"

#include "sim/stream_nuca/stream_nuca_manager.hh"
#include "sim/stream_nuca/stream_nuca_map.hh"

#include "debug/StreamPUM.hh"

#define DEBUG_TYPE StreamPUM
#include "../../stream_log.hh"

#define MLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(StreamPUM, "[MLC_SE%d]: [PUM] " format,                              \
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

MLCPUMManager::MLCPUMManager(MLCStreamEngine *_mlcSE)
    : mlcSE(_mlcSE), controller(_mlcSE->controller) {}

MLCPUMManager::~MLCPUMManager() {}

void MLCPUMManager::PUMContext::clear() {
  this->configuredBanks = 0;
  this->totalSentPackets = 0;
  this->totalRecvPackets = 0;
  this->totalAckBanks = 0;
  this->reachedSync = 0;
  this->totalSyncs = 0;
  /**
   * Don't forget to release the commands.
   */
  this->commands.clear();
}

void MLCPUMManager::findPUMComputeStreamGroups(PUMContext &context) {
  for (const auto &config : context.configs) {
    bool shouldFormGroup = false;
    CacheStreamConfigureDataPtr reduceConfig = nullptr;
    CacheStreamConfigureDataPtr realComputeConfig = config;
    if (config->stream->getEnabledStoreFunc()) {
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
      group.usedConfigs.push_back(baseConfig);
    }
  }
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
  auto checkCommonConstraint = [&](const ConfigPtr &config) -> bool {
    if (!config->stream->isLoopEliminated()) {
      MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[NoPUM] Not Eliminated.\n");
      return false;
    }
    for (const auto &dep : config->depEdges) {
      if (dep.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {

        /**
         * We try to support distributable reduction here.
         */
        if (dep.data->stream->isReduction()) {
          if (!dep.data->stream->isReductionDistributable()) {
            MLC_S_DPRINTF_(StreamPUM, config->dynamicId,
                           "[NoPUM] Reduce Not Distributable %s.\n",
                           dep.data->dynamicId);
            return false;
          }
        } else {
          MLC_S_DPRINTF_(StreamPUM, config->dynamicId,
                         "[NoPUM] Has IndirectS %s.\n", dep.data->dynamicId);
          return false;
        }
      }
    }
    if (config->floatPlan.isFloatedToMem()) {
      MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[NoPUM] Float to Mem.\n");
      return false;
    }
    if (config->floatPlan.getFirstFloatElementIdx() != 0) {
      MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[NoPUM] Delayed Float.\n");
      return false;
    }

    if (!config->hasTotalTripCount()) {
      MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[NoPUM] No TripCount.\n");
      return false;
    }
    auto linearAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
        config->addrGenCallback);
    if (!linearAddrGen) {
      MLC_S_DPRINTF_(StreamPUM, config->dynamicId,
                     "[NoPUM] Not LinearAddrGen.\n");
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
      MLC_S_DPRINTF_(StreamPUM, config->dynamicId,
                     "[NoPUM] Fault StartVAddr.\n");
      return false;
    }
    auto rangeMap = StreamNUCAMap::getRangeMapContaining(startPAddr);
    if (!rangeMap) {
      MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[NoPUM] No RangeMap.\n");
      return false;
    }
    if (!rangeMap->isStreamPUM) {
      MLC_S_DPRINTF_(StreamPUM, config->dynamicId,
                     "[NoPUM] RangeMap not PUM.\n");
      return false;
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
        context.patternInfo
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

    return true;
  };

  const auto &computeConfig = group.computeConfig;
  const auto &groupDynId = computeConfig->dynamicId;
  if (!checkCommonConstraint(computeConfig)) {
    return false;
  }

  for (const auto &baseConfig : group.usedConfigs) {
    if (!baseConfig->baseEdges.empty()) {
      MLC_S_DPRINTF_(StreamPUM, groupDynId, "[NoPUM] UsedS with BaseEdge %s.\n",
                     baseConfig->dynamicId);
      return false;
    }
    if (!checkCommonConstraint(baseConfig)) {
      return false;
    }
  }

  // All regions should have the same tile mapping.
  const auto &computeTile =
      context.patternInfo.at(computeConfig->stream).pumTile;
  for (const auto &baseConfig : group.usedConfigs) {
    const auto &tile = context.patternInfo.at(baseConfig->stream).pumTile;
    if (tile != computeTile) {
      MLC_S_DPRINTF(groupDynId, "[NoPUM] Mismatch Tile %s and %s from %s.\n",
                    computeTile, tile, baseConfig->dynamicId);
      return false;
    }
  }

  // Before asking DataMoveCompiler, we need some preprocessing on the stream
  // patterns to at least try to make it suitable for PUM.
  this->preprocessPatternsInGroup(context, group);

  // Check for DataMoveCompiler.
  for (const auto &sendConfig : group.usedConfigs) {
    for (const auto &dep : sendConfig->depEdges) {
      if (dep.type != CacheStreamConfigureData::DepEdge::Type::SendTo) {
        continue;
      }
      if (dep.data != group.computeConfig) {
        // Not to us.
        continue;
      }

      const auto &sendDynId = sendConfig->dynamicId;
      auto S = sendConfig->stream;
      const auto &patternInfo = context.patternInfo.at(S);
      auto myTile = patternInfo.pumTile;

      MLC_S_DPRINTF(sendDynId, "[PUM] --- Can Compile DataMove. MyTile %s.\n",
                    myTile);
      DataMoveCompiler compiler(PUMHWConfiguration::getPUMHWConfig(), myTile);

      auto recvConfig = dep.data;
      auto recvS = recvConfig->stream;
      const auto &recvPatternInfo = context.patternInfo.at(recvS);
      auto recvTile = recvPatternInfo.pumTile;

      if (recvTile != myTile) {
        // TODO: Handle different mapping of source and dest stream.
        MLC_S_DPRINTF(groupDynId,
                      "[NoPUM] Mismatch RecvTile %s and SendTile %s.\n",
                      recvTile, myTile);
        return false;
      }

      if (recvPatternInfo.atomicPatterns.size() != 1) {
        MLC_S_DPRINTF(groupDynId, "[NoPUM] Multi RecvPatterns.\n", recvTile,
                      myTile);
        return false;
      }
      const auto &recvPattern = recvPatternInfo.atomicPatterns.front();
      MLC_S_DPRINTF(recvConfig->dynamicId, "[PUM] RecvPattern %s.\n",
                    recvPattern);

      for (const auto &myPattern : patternInfo.atomicPatterns) {
        MLC_S_DPRINTF(sendDynId, "[PUM] SendPattern %s.\n", myPattern);

        auto reusedMyPattern =
            this->addReuseToOuterPattern(sendConfig, recvConfig, myPattern);

        if (!compiler.canCompileStreamPair(reusedMyPattern, recvPattern)) {
          MLC_S_DPRINTF(groupDynId, "[NoPUM] Rejected by DataMoveCompiler.\n");
          return false;
        }
      }
    }
  }

  return true;
}

void MLCPUMManager::compileContext(PUMContext &context) {

  for (auto &group : context.pumGroups) {
    if (group.appliedPUM) {
      this->compileGroup(context, group);
    }
  }

  // Remember number of syncs.
  for (const auto &command : context.commands) {
    if (command.type == "sync") {
      context.totalSyncs++;
    }
  }
  // Implicit last sync.
  context.totalSyncs++;
}

void MLCPUMManager::compileGroup(PUMContext &context,
                                 PUMComputeStreamGroup &group) {

  assert(group.canApplyPUM);

  for (const auto &sendConfig : group.usedConfigs) {
    for (const auto &dep : sendConfig->depEdges) {
      if (dep.type != CacheStreamConfigureData::DepEdge::Type::SendTo) {
        continue;
      }
      if (dep.data != group.computeConfig) {
        // Not to us.
        continue;
      }
      this->compileDataMove(context, group, sendConfig);
    }
  }

  /**
   * Insert sync command after all data movement.
   * This is default enabled for every LLC bank.
   */
  context.commands.emplace_back();
  auto &sync = context.commands.back();
  sync.type = "sync";

  this->compileCompute(context, group);

  /**
   * We try to increment the OuterIter.
   */
  group.nextOuterIter++;
}

void MLCPUMManager::erasePUMConfigs(PUMContext &context,
                                    CacheStreamConfigureVec *configs,
                                    const PUMComputeStreamGroup &group) {

  auto eraseFromNormalConfigs = [&](const ConfigPtr &target) -> void {
    bool erased = false;
    for (auto iter = configs->begin(); iter != configs->end(); ++iter) {
      const auto &config = *iter;
      if (config == target) {
        context.purePUMStreamIds.push_back(target->dynamicId);
        configs->erase(iter);
        erased = true;
        break;
      }
    }
    assert(erased);
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

  const auto &patInfo = context.patternInfo.at(directConfig->stream);

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
  newDirectConfig->pumElemPerSync = tileAlignedAtomicPat.get_total_trip();
  newDirectConfig->hintNoStrandSplitOuterTripCount = 1;
  if (!patInfo.splitOuterDims.empty()) {
    const auto &splitDims = patInfo.splitOuterDims.front();
    tileAlignedAtomicPat.params.insert(tileAlignedAtomicPat.params.end(),
                                       splitDims.params.begin(),
                                       splitDims.params.end());
    newDirectConfig->hintNoStrandSplitOuterTripCount =
        splitDims.get_total_trip();
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
  newDirectConfig->totalTripCount = tileAlignedAtomicPat.get_total_trip();
  newDirectConfig->innerTripCount = newInnerTripCount;
  newReduceConfig->totalTripCount = tileAlignedAtomicPat.get_total_trip();
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

void MLCPUMManager::receiveStreamConfigure(PacketPtr pkt) {

  if (this->controller->myParams->stream_pum_mode != 1) {
    return;
  }

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
    return;
  }

  // Record the initialize cycle.
  context.initCycle = this->controller->curCycle();

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

  // Really compile for the first time.
  this->compileContext(context);

  if (this->contexts.size() == 1) {
    // Start PUM only if we reached the front of the queue.
    this->configurePUMEngine(context);
  }

  return;
}

void MLCPUMManager::configurePUMEngine(PUMContext &context) {
  for (int row = 0; row < this->controller->getNumRows(); ++row) {
    for (int col = 0; col < this->controller->getNumCols(); ++col) {
      int nodeId = row * this->controller->getNumCols() + col;
      MachineID dstMachineId(MachineType_L2Cache, nodeId);

      context.configuredBanks++;

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
  context.state = PUMContext::StateE::Kicked;
  context.lastKickCycle = this->controller->curCycle();
  this->kickPUMEngine(context, MessageSizeType_Data, false /* isIdea */);
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

void MLCPUMManager::compileDataMove(PUMContext &context,
                                    PUMComputeStreamGroup &group,
                                    const ConfigPtr &sendConfig) {

  const auto &sendDynId = sendConfig->dynamicId;
  auto S = sendConfig->stream;
  const auto &patInfo = context.patternInfo.at(S);
  auto myTile = patInfo.pumTile;

  MLC_S_DPRINTF(sendDynId, "[PUM] --- Compile DataMove. MyTile %s.\n", myTile);
  DataMoveCompiler compiler(PUMHWConfiguration::getPUMHWConfig(), myTile);

  auto recvConfig = group.computeConfig;
  auto recvS = recvConfig->stream;
  const auto &recvPatInfo = context.patternInfo.at(recvS);
  auto recvTile = recvPatInfo.pumTile;

  if (recvTile != myTile) {
    // TODO: Handle different mapping of source and dest stream.
    MLC_S_PANIC_NO_DUMP(sendDynId, "[PUM] Different Tile.");
  }

  if (recvPatInfo.atomicPatterns.size() != 1) {
    MLC_S_PANIC_NO_DUMP(recvConfig->dynamicId, "[PUM] Multi Recv.");
  }
  auto recvPat =
      recvPatInfo.getPatternAdjustedByOuterIter(0, group.nextOuterIter);
  MLC_S_DPRINTF(recvConfig->dynamicId, "[PUM] OuterIter %ld RecvPattern %s.\n",
                group.nextOuterIter, recvPat);

  for (auto patIdx = 0; patIdx < patInfo.atomicPatterns.size(); ++patIdx) {

    auto sendPat =
        patInfo.getPatternAdjustedByOuterIter(patIdx, group.nextOuterIter);

    MLC_S_DPRINTF(sendDynId, "[PUM] OuterIter %ld SendPattern %s.\n",
                  group.nextOuterIter, sendPat);

    auto reusedMyPattern =
        this->addReuseToOuterPattern(sendConfig, recvConfig, sendPat);

    auto commands = compiler.compile(reusedMyPattern, recvPat);
    // Generate the meta information.
    for (auto &cmd : commands) {
      cmd.wordline_bits = patInfo.scalarElemSize * 8;
      cmd.dynStreamId = sendConfig->dynamicId;
      cmd.srcRegion = patInfo.regionName;
      cmd.srcAccessPattern = reusedMyPattern;
      cmd.srcMapPattern = myTile;
      cmd.dstRegion = recvPatInfo.regionName;
      cmd.dstAccessPattern = recvPat;
      cmd.dstMapPattern = myTile;
    }
    if (Debug::StreamPUM) {
      for (const auto &command : commands) {
        MLC_S_DPRINTF(sendDynId, "%s", command);
      }
    }
    context.commands.insert(context.commands.end(), commands.begin(),
                            commands.end());
  }
}

void MLCPUMManager::compileCompute(PUMContext &context,
                                   PUMComputeStreamGroup &group) {

  const auto &config = group.computeConfig;
  const auto &dynId = config->dynamicId;

  auto &patInfo = context.patternInfo.at(config->stream);
  auto scalarElemBits = patInfo.scalarElemSize * 8;

  DataMoveCompiler compiler(PUMHWConfiguration::getPUMHWConfig(),
                            patInfo.pumTile);

  PUMCommandVecT commands;

  assert(patInfo.atomicPatterns.size() == 1);
  auto pattern = patInfo.getPatternAdjustedByOuterIter(0, group.nextOuterIter);

  MLC_S_DPRINTF(dynId,
                "Compile Compute Tile %s OuterIter %ld AdjustedPattern %s.\n",
                patInfo.pumTile, group.nextOuterIter, pattern);

  ExecFuncPtr func = nullptr;
  if (group.reduceConfig) {
    auto funcAddrGenCb = std::dynamic_pointer_cast<FuncAddrGenCallback>(
        group.reduceConfig->addrGenCallback);
    if (!funcAddrGenCb) {
      MLC_S_PANIC_NO_DUMP(group.reduceConfig->dynamicId,
                          "[PUM] Reduction should have FuncAddrGenCallback.");
    }
    func = funcAddrGenCb->getExecFunc();
  } else {
    func = config->storeCallback;
  }
  if (!func) {
    MLC_S_PANIC_NO_DUMP(dynId, "[PUM] Failed to find ComputeFunc.");
  }

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

    MLC_S_DPRINTF(dynId, "[PUM] Compile Inst %s to OpClass %s.\n",
                  inst->disassemble(0x0),
                  Enums::OpClassStrings[inst->opClass()]);
  }

  // Compile the final reduction instruction.
  this->compileReduction(context, group, commands);

  if (Debug::StreamPUM) {
    for (const auto &command : commands) {
      MLC_S_DPRINTF(config->dynamicId, "%s", command);
    }
  }
  MLC_S_DPRINTF(config->dynamicId, "Before masked.\n");

  // Mask the commands by the Stream.
  commands = compiler.maskCmdsBySubRegion(commands, pattern);

  if (Debug::StreamPUM) {
    for (const auto &command : commands) {
      MLC_S_DPRINTF(config->dynamicId, "%s", command);
    }
  }
  MLC_S_DPRINTF(config->dynamicId, "Before mapped to LLC.\n");

  // Generate mask for each LLC bank.
  commands = compiler.mapCmdsToLLC(commands);

  // Generate the meta information.
  for (auto &cmd : commands) {
    cmd.wordline_bits = scalarElemBits;
    cmd.dynStreamId = config->dynamicId;
    cmd.srcRegion = patInfo.regionName;
    cmd.srcAccessPattern = patInfo.pattern;
    cmd.srcMapPattern = patInfo.pumTile;
  }
  if (Debug::StreamPUM) {
    for (const auto &command : commands) {
      MLC_S_DPRINTF(config->dynamicId, "%s", command);
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

  const auto &patInfo = context.patternInfo.at(group.computeConfig->stream);
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
    // Generate all active bitline-mask according to tile_sizes.
    commands.back().bitline_mask = AffinePattern::constructSubRegion(
        tileSizes, AffinePattern::IntVecT(tileSizes.size(), 0), tileSizes);

    MLC_S_DPRINTF(reduceDynId, "Intra-Array Reduce Cmd %s", commands.back());

    // We then insert the reduce compute command.
    commands.push_back(reduceCmd);

    curRatio *= 2;
  }
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
  auto &recvPatInfo = context.patternInfo.at(recvS);
  auto recvTile = recvPatInfo.pumTile;

  if (recvPatInfo.atomicPatterns.size() != 1) {
    MLC_S_DPRINTF(groupDynId, "[NoPUM] Multi RecvPatterns.\n");
    return;
  }

  MLC_S_DPRINTF(groupDynId, "[PUM] Preprocess Patterns in Group.\n");

  for (const auto &sendConfig : group.usedConfigs) {

    const auto &sendDynId = sendConfig->dynamicId;
    auto S = sendConfig->stream;
    auto &sendPatInfo = context.patternInfo.at(S);

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
  for (const auto &sendConfig : group.usedConfigs) {

    const auto &sendDynId = sendConfig->dynamicId;
    auto S = sendConfig->stream;
    auto &sendPatInfo = context.patternInfo.at(S);

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
  for (const auto &sendConfig : group.usedConfigs) {

    const auto &sendDynId = sendConfig->dynamicId;
    auto S = sendConfig->stream;
    auto &sendPatInfo = context.patternInfo.at(S);
    for (auto &sendPat : sendPatInfo.atomicPatterns) {

      auto splitPat = sendPat.splitFromDim(arrayDims);
      MLC_S_DPRINTF(sendDynId, "[PUM]     SendPat Split into %s %s.\n", sendPat,
                    splitPat);

      sendPatInfo.splitOuterDims.push_back(splitPat);
    }
  }

  auto recvSplitPat = recvPat.splitFromDim(arrayDims);
  MLC_S_DPRINTF(groupDynId, "[PUM]     RecvPat Split into %s %s.\n", recvPat,
                recvSplitPat);

  recvPatInfo.splitOuterDims.push_back(recvSplitPat);
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
  context.totalAckBanks++;
  context.totalSentPackets += sentPackets;
  this->checkSync(context);
}

void MLCPUMManager::receivePacket(int recvPackets) {
  auto &context = this->getFirstKickedContext();
  assert(context.isActive() && "No Active PUM.");
  context.totalRecvPackets += recvPackets;
  this->checkSync(context);
}

void MLCPUMManager::checkSync(PUMContext &context) {

  assert(context.isActive() && "No Active PUM.");

  if (context.reachedSync == context.totalSyncs) {
    return;
  }

  if (context.totalAckBanks == context.configuredBanks &&
      context.totalSentPackets == context.totalRecvPackets) {

    MLCSE_DPRINTF("Synced %d Total %d.\n", context.reachedSync,
                  context.totalSyncs);
    context.reachedSync++;
    context.totalAckBanks = 0;
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
    auto dynS = S->getDynStream(config->dynamicId);
    if (!dynS) {
      MLC_S_PANIC_NO_DUMP(config->dynamicId, "No CoreDynS.");
    }
    if (dynS->shouldCoreSEIssue()) {
      MLC_S_PANIC_NO_DUMP(config->dynamicId,
                          "CoreSE should not issue for PUM.");
    }
    if (S->isStoreComputeStream() || S->isAtomicComputeStream() ||
        S->isUpdateStream()) {
      // These are streams waiting for Ack.
      assert(config->hasTotalTripCount());
      auto tripCount = config->getTotalTripCount();
      for (int64_t elemIdx = 0; elemIdx < tripCount; ++elemIdx) {
        dynS->cacheAckedElements.insert(elemIdx);
      }
    }
    if (group.reduceConfig) {
      // this->completeFinalReduction(context, group);
    }

    auto outerTripCount = 1;
    if (group.outerDimSplitted) {
      const auto &patInfo = context.patternInfo.at(S);
      assert(patInfo.splitOuterDims.size() == 1);
      outerTripCount = patInfo.splitOuterDims.front().get_total_trip();
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

        return;
      }
    }
  }

  // We can start next round.
  context.clear();
  context.state = PUMContext::StateE::Initialized;
  this->compileContext(context);
  this->configurePUMEngine(context);
}

void MLCPUMManager::receiveStreamEnd(PacketPtr pkt) {

  auto &endIds = **(pkt->getPtr<std::vector<DynStreamId> *>());

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

  const auto &patInfo = context.patternInfo.at(group.computeConfig->stream);
  assert(patInfo.atomicPatterns.size() == 1);
  auto reducedTripCount = patInfo.atomicPatterns.front().get_total_trip();

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