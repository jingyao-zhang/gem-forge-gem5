#include "MLCPUMManager.hh"
#include "PUMEngine.hh"

#include "../LLCStreamEngine.hh"

#include "sim/stream_nuca/stream_nuca_manager.hh"
#include "sim/stream_nuca/stream_nuca_map.hh"

#include "debug/StreamPUM.hh"

#define DEBUG_TYPE StreamPUM
#include "../../stream_log.hh"

#define MLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(StreamPUM, "[MLC_SE%d]: [PUM] " format,                              \
          this->controller->getMachineID().num, ##args)

MLCPUMManager::MLCPUMManager(MLCStreamEngine *_mlcSE)
    : mlcSE(_mlcSE), controller(_mlcSE->controller) {}

MLCPUMManager::~MLCPUMManager() {}

void MLCPUMManager::PUMContext::clear() {
  this->configuredBanks = 0;
  this->totalSentPackets = 0;
  this->totalRecvPackets = 0;
  this->totalAckBanks = 0;
  this->numSync = 0;
  /**
   * Don't forget to release the ConfigVec.
   */
  this->configs.clear();
}

void MLCPUMManager::findPUMComputeStreamGroups(CompileStates &states) {
  for (const auto &config : states.configs) {
    if (config->stream->getEnabledStoreFunc()) {
      states.pumGroups.emplace_back();

      auto &group = states.pumGroups.back();
      group.computeConfig = config;
      for (const auto &edge : config->baseEdges) {
        auto baseConfig = edge.data.lock();
        assert(baseConfig && "BaseConfig already released.");
        group.usedConfigs.push_back(baseConfig);
      }
    }
  }
}

bool MLCPUMManager::canApplyPUMToGroup(CompileStates &states,
                                       const PUMComputeStreamGroup &group) {
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
        MLC_S_DPRINTF_(StreamPUM, config->dynamicId,
                       "[NoPUM] Has IndirectS %s.\n", dep.data->dynamicId);
        return false;
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
        states.patternInfo
            .emplace(std::piecewise_construct, std::forward_as_tuple(S),
                     std::forward_as_tuple())
            .first->second;
    patternInfo.regionName = streamNUCARegion.name;
    patternInfo.pumTile = rangeMap->pumTile;
    patternInfo.pattern = pattern;
    patternInfo.scalarElemSize = scalarElemSize;
    patternInfo.atomicPatterns =
        this->decoalesceAndDevectorizePattern(config, pattern, scalarElemSize);

    /**
     * TODO: Additional check that pattern is sub-region.
     */
    return true;
  };

  const auto &computeConfig = group.computeConfig;
  const auto &groupDynId = computeConfig->dynamicId;
  if (!computeConfig->depEdges.empty()) {
    MLC_S_DPRINTF_(StreamPUM, groupDynId, "[NoPUM] Has IndirectS %s.\n",
                   computeConfig->depEdges.front().data->dynamicId);
    return false;
  }

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

  // Final check: all regions should have the same tile mapping.
  const auto &computeTile =
      states.patternInfo.at(computeConfig->stream).pumTile;
  for (const auto &baseConfig : group.usedConfigs) {
    const auto &tile = states.patternInfo.at(baseConfig->stream).pumTile;
    if (tile != computeTile) {
      MLC_S_DPRINTF_(StreamPUM, groupDynId,
                     "[NoPUM] Mismatch Tile %s and %s from %s.\n", computeTile,
                     tile, baseConfig->dynamicId);
      return false;
    }
  }

  return true;
}

void MLCPUMManager::applyPUMToGroup(CompileStates &states,
                                    PUMComputeStreamGroup &group) {

  assert(group.canApplyPUM);
  group.appliedPUM = true;
  for (const auto &sendConfig : group.usedConfigs) {
    for (const auto &dep : sendConfig->depEdges) {
      if (dep.type != CacheStreamConfigureData::DepEdge::Type::SendTo) {
        continue;
      }
      if (dep.data != group.computeConfig) {
        // Not to us.
        continue;
      }
      this->compileDataMove(states, sendConfig, dep);
    }
  }

  /**
   * Insert sync command after all data movement.
   * This is default enabled for every LLC bank.
   */
  states.commands.emplace_back();
  auto &sync = states.commands.back();
  sync.type = "sync";

  this->compileCompute(states, group.computeConfig);
}

void MLCPUMManager::receiveStreamConfigure(PacketPtr pkt) {

  if (this->controller->myParams->stream_pum_mode != 1) {
    return;
  }

  CompileStates states;
  // Take a copy of the configure vector.
  states.configs = **(pkt->getPtr<CacheStreamConfigureVec *>());
  this->findPUMComputeStreamGroups(states);
  for (auto &group : states.pumGroups) {
    group.canApplyPUM = this->canApplyPUMToGroup(states, group);
  }
  bool appliedPUM = false;
  for (auto &group : states.pumGroups) {
    if (group.canApplyPUM) {
      this->applyPUMToGroup(states, group);
      appliedPUM = true;
    }
  }

  if (!appliedPUM) {
    return;
  }

  assert(!this->context.isActive() && "PUM Already Active.");
  // Remember the configured streams.
  this->context.configs = states.configs;
  this->context.pumGroups = states.pumGroups;
  this->context.commands = states.commands;
  for (const auto &command : states.commands) {
    if (command.type == "sync") {
      this->context.numSync++;
    }
  }
  // Implicit last sync.
  this->context.numSync++;

  this->configurePUMEngine(states);

  /**
   * Clear the PUMConfigs from the original ConfigVec so that MLCStreamEngine
   * can continue to handle normal streams.
   */
  auto normalConfigs = *(pkt->getPtr<CacheStreamConfigureVec *>());
  auto eraseFromNormalConfigs = [&](const ConfigPtr &target) -> void {
    bool erased = false;
    for (auto iter = normalConfigs->begin(); iter != normalConfigs->end();
         ++iter) {
      const auto &config = *iter;
      if (config == target) {
        this->context.purePUMStreamIds.push_back(target->dynamicId);
        normalConfigs->erase(iter);
        erased = true;
        break;
      }
    }
    assert(erased);
  };
  auto findConfig = [&](Stream *S) -> ConfigPtr {
    for (auto iter = normalConfigs->begin(); iter != normalConfigs->end();
         ++iter) {
      const auto &config = *iter;
      if (config->stream == S) {
        return config;
      }
    }
    assert(false && "Failed to find Config for Stream.");
  };
  for (const auto &group : states.pumGroups) {
    if (!group.appliedPUM) {
      continue;
    }
    MLC_S_DPRINTF(group.computeConfig->dynamicId,
                  "[PUMErased] Erase ComputeConfig.\n");
    eraseFromNormalConfigs(group.computeConfig);
  }
  for (const auto &compiledDataMove : states.compiledDataMove) {
    auto sendS = compiledDataMove.first;
    auto recvS = compiledDataMove.second;
    auto sendConfig = findConfig(sendS);
    auto &deps = sendConfig->depEdges;
    bool erased = false;
    for (auto iter = deps.begin(); iter != deps.end(); ++iter) {
      if (iter->data->stream == recvS) {
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

  return;
}

void MLCPUMManager::configurePUMEngine(CompileStates &states) {
  for (int row = 0; row < this->controller->getNumRows(); ++row) {
    for (int col = 0; col < this->controller->getNumCols(); ++col) {
      int nodeId = row * this->controller->getNumCols() + col;
      MachineID dstMachineId(MachineType_L2Cache, nodeId);

      this->context.configuredBanks++;

      /**
       * We still configure here. But PUMEngine will not start until received
       * the Kick.
       */
      auto llcCntrl =
          AbstractStreamAwareController::getController(dstMachineId);
      auto llcSE = llcCntrl->getLLCStreamEngine();
      llcSE->getPUMEngine()->configure(this, states.commands);
    }
  }
  this->kickPUMEngine(MessageSizeType_Data);
}

void MLCPUMManager::kickPUMEngine(MessageSizeType sizeType) {

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

void MLCPUMManager::compileDataMove(
    CompileStates &states, const ConfigPtr &sendConfig,
    const CacheStreamConfigureData::DepEdge &dep) {

  const auto &sendDynId = sendConfig->dynamicId;
  auto S = sendConfig->stream;
  const auto &patternInfo = states.patternInfo.at(S);
  auto myTile = patternInfo.pumTile;

  MLC_S_DPRINTF(sendDynId, "[PUM] --- Compile DataMove. MyTile %s.\n", myTile);
  DataMoveCompiler compiler(PUMHWConfiguration::getPUMHWConfig(), myTile);

  assert(dep.type == CacheStreamConfigureData::DepEdge::Type::SendTo);

  auto recvConfig = dep.data;
  auto recvS = recvConfig->stream;
  const auto &recvPatternInfo = states.patternInfo.at(recvS);
  auto recvTile = recvPatternInfo.pumTile;

  assert(states.compiledDataMove.emplace(S, recvS).second &&
         "CompiledDataMove Twice.");

  if (recvTile != myTile) {
    // TODO: Handle different mapping of source and dest stream.
    MLC_S_PANIC_NO_DUMP(sendDynId, "[PUM] Different Tile.");
  }

  if (recvPatternInfo.atomicPatterns.size() != 1) {
    MLC_S_PANIC_NO_DUMP(recvConfig->dynamicId, "[PUM] Multi Recv.");
  }
  const auto &recvPattern = recvPatternInfo.atomicPatterns.front();
  MLC_S_DPRINTF(recvConfig->dynamicId, "[PUM] RecvPattern %s.\n", recvPattern);

  for (const auto &myPattern : patternInfo.atomicPatterns) {
    MLC_S_DPRINTF(sendDynId, "[PUM] SendPattern %s.\n", myPattern);

    auto reusedMyPattern =
        this->addReuseToOuterPattern(sendConfig, recvConfig, myPattern);

    auto commands = compiler.compile(reusedMyPattern, recvPattern);
    // Generate the meta information.
    for (auto &cmd : commands) {
      cmd.wordline_bits = patternInfo.scalarElemSize * 8;
      cmd.dynStreamId = sendConfig->dynamicId;
      cmd.srcRegion = patternInfo.regionName;
      cmd.srcAccessPattern = reusedMyPattern;
      cmd.srcMapPattern = myTile;
      cmd.dstRegion = recvPatternInfo.regionName;
      cmd.dstAccessPattern = recvPattern;
      cmd.dstMapPattern = myTile;
    }
    if (Debug::StreamPUM) {
      for (const auto &command : commands) {
        MLC_S_DPRINTF(sendDynId, "%s", command);
      }
    }
    states.commands.insert(states.commands.end(), commands.begin(),
                           commands.end());
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

void MLCPUMManager::reachSync(int sentPackets) {
  assert(this->context.isActive() && "No Active PUM.");
  this->context.totalAckBanks++;
  this->context.totalSentPackets += sentPackets;
  this->checkSync();
}

void MLCPUMManager::receivePacket() {
  assert(this->context.isActive() && "No Active PUM.");
  this->context.totalRecvPackets++;
  this->checkSync();
}

void MLCPUMManager::checkSync() {

  assert(this->context.isActive() && "No Active PUM.");

  if (this->context.numSync == 0) {
    return;
  }

  if (this->context.totalAckBanks == this->context.configuredBanks &&
      this->context.totalSentPackets == this->context.totalRecvPackets) {

    MLCSE_DPRINTF("Synced %d -= 1.\n", this->context.numSync);
    this->context.numSync--;
    this->context.totalAckBanks = 0;
    this->context.totalSentPackets = 0;
    this->context.totalRecvPackets = 0;

    if (this->context.numSync == 0) {
      // This is the last Sync.
      MLCSE_DPRINTF("Ack all elements at core.\n");
      for (const auto &group : context.pumGroups) {
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
      }
    } else {
      // Notify the PUMEngine to continue.
      for (int row = 0; row < this->controller->getNumRows(); ++row) {
        for (int col = 0; col < this->controller->getNumCols(); ++col) {
          int nodeId = row * this->controller->getNumCols() + col;
          MachineID dstMachineId(MachineType_L2Cache, nodeId);

          /**
           * We still configure here. But PUMEngine will not start until
           * received the configuration message.
           */
          auto llcCntrl =
              AbstractStreamAwareController::getController(dstMachineId);
          auto llcSE = llcCntrl->getLLCStreamEngine();
          llcSE->getPUMEngine()->synced();
        }
      }
    }
  }
}

void MLCPUMManager::receiveStreamEnd(PacketPtr pkt) {
  if (!this->context.isActive()) {
    return;
  }

  auto endIds = *(pkt->getPtr<std::vector<DynStreamId> *>());
  for (auto &dynId : this->context.purePUMStreamIds) {
    for (auto iter = endIds->begin(); iter != endIds->end(); ++iter) {
      if (dynId == (*iter)) {
        endIds->erase(iter);
        break;
      }
    }
  }

  assert(this->context.numSync == 0 && "PUM end before done.");
  MLCSE_DPRINTF("Release PUM context.\n");
  this->context.clear();
}

void MLCPUMManager::compileCompute(CompileStates &states,
                                   const ConfigPtr &config) {
  if (!config->storeCallback) {
    return;
  }

  auto &patternInfo = states.patternInfo.at(config->stream);
  auto scalarElemBits = patternInfo.scalarElemSize * 8;

  DataMoveCompiler compiler(PUMHWConfiguration::getPUMHWConfig(),
                            patternInfo.pumTile);

  PUMCommandVecT commands;

  assert(patternInfo.atomicPatterns.size() == 1);
  const auto &pattern = patternInfo.atomicPatterns.front();

  MLC_S_DPRINTF(config->dynamicId,
                "Compile Compute Tile %s AtomicPattern %s.\n",
                patternInfo.pumTile, pattern);

  for (const auto &inst : config->storeCallback->getStaticInsts()) {

    commands.emplace_back();
    auto &command = commands.back();
    command.type = "cmp";
    command.opClass = inst->opClass();
    // Default bitline_mask is for the entire tile.
    command.bitline_mask = AffinePattern::construct_canonical_sub_region(
        compiler.tile_sizes,
        AffinePattern::IntVecT(compiler.tile_sizes.size(), 0) /* starts */,
        compiler.tile_sizes);

    DPRINTF(StreamPUM, "[PUM] Compile Inst %s to OpClass %s.\n",
            inst->disassemble(0x0), Enums::OpClassStrings[inst->opClass()]);
  }

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
    cmd.srcRegion = patternInfo.regionName;
    cmd.srcAccessPattern = patternInfo.pattern;
    cmd.srcMapPattern = patternInfo.pumTile;
  }
  if (Debug::StreamPUM) {
    for (const auto &command : commands) {
      MLC_S_DPRINTF(config->dynamicId, "%s", command);
    }
  }

  states.commands.insert(states.commands.end(), commands.begin(),
                         commands.end());
}