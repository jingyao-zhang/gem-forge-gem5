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
  this->done = false;
  /**
   * Don't forget to release the ConfigVec.
   */
  delete this->configs;
  this->configs = nullptr;
}

bool MLCPUMManager::canApplyPUM(Args &args) {

  /**
   * We can only apply PUM iff:
   * 1. Only affine streams (no reduction or pointer-chasing).
   * 2. One StoreComputeStream and Some forwarding streams.
   * 3. Loop is eliminated.
   * 4. Known trip count.
   * 5. For stream patterns:
   *    StoreComputeStream must be a sub-region.
   *    LoadForwardStream must be able to reduce to a sub-region,
   *    with matched dimension with the StoreComputeStream.
   * 6. TODO: Enough wordlines to hold inputs and intermediate data.
   */

  for (const auto &config : *args.configs) {
    if (!this->canApplyPUM(args, config)) {
      return false;
    }
  }

  if (args.numStoreStreams == 0) {
    MLCSE_DPRINTF("[PUM] No StoreStream.\n");
    return false;
  }
  return true;
}

bool MLCPUMManager::canApplyPUM(Args &args,
                                const CacheStreamConfigureDataPtr &config) {

  if (!config->stream->isLoopEliminated()) {
    MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[PUM] Not Eliminated.\n");
    return false;
  }
  if (config->stream->isStoreComputeStream()) {
    args.numStoreStreams++;
    if (args.numStoreStreams > 1) {
      MLC_S_DPRINTF_(StreamPUM, config->dynamicId,
                     "[PUM] Multi StoreStream.\n");
      return false;
    }
  }
  for (const auto &dep : config->depEdges) {
    if (dep.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {
      MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[PUM] Has IndirectS %s.\n",
                     dep.data->dynamicId);
      return false;
    }
  }
  if (config->floatPlan.isFloatedToMem()) {
    MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[PUM] Float to Mem.\n");
    return false;
  }
  if (config->floatPlan.getFirstFloatElementIdx() != 0) {
    MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[PUM] Delayed Float.\n");
    return false;
  }

  if (!config->hasTotalTripCount()) {
    MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[PUM] No TripCount.\n");
    return false;
  }
  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(config->addrGenCallback);
  if (!linearAddrGen) {
    MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[PUM] Not LinearAddrGen.\n");
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
    MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[PUM] Fault StartVAddr.\n");
    return false;
  }
  auto rangeMap = StreamNUCAMap::getRangeMapContaining(startPAddr);
  if (!rangeMap) {
    MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[PUM] No RangeMap.\n");
    return false;
  }
  if (!rangeMap->isStreamPUM) {
    MLC_S_DPRINTF_(StreamPUM, config->dynamicId, "[PUM] RangeMap not PUM.\n");
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
      args.patternInfo
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
}

/**
 * Receive a StreamConfig message and generate PUM commands.
 */
bool MLCPUMManager::receiveStreamConfigure(PacketPtr pkt) {

  if (!this->controller->myParams->enable_stream_pum) {
    return false;
  }

  Args args;
  args.configs = *(pkt->getPtr<CacheStreamConfigureVec *>());
  if (!this->canApplyPUM(args)) {
    return false;
  }

  this->applyPUM(args);

  // Do not release the configure vec as it's remembered in Context.
  delete pkt;
  return true;
}

void MLCPUMManager::applyPUM(Args &args) {
  for (const auto &config : *args.configs) {
    this->compileDataMove(args, config);
  }

  assert(!this->context.isActive() && "PUM Already Active.");
  // Remember the configured streams.
  this->context.configs = args.configs;
  this->context.commands = args.commands;

  this->configurePUMEngine(args);
}

void MLCPUMManager::configurePUMEngine(Args &args) {

  /**
   * Broadcast the configure packet.
   */
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = 0;
  msg->m_Type = CoherenceRequestType_STREAM_CONFIG;
  msg->m_Requestors.add(this->controller->getMachineID());
  msg->m_MessageSize = MessageSizeType_Data;
  msg->m_isPUM = true;

  for (int row = 0; row < this->controller->getNumRows(); ++row) {
    for (int col = 0; col < this->controller->getNumCols(); ++col) {
      int nodeId = row * this->controller->getNumCols() + col;
      MachineID dstMachineId(MachineType_L2Cache, nodeId);

      msg->m_Destination.add(dstMachineId);
      this->context.configuredBanks++;

      /**
       * We still configure here. But PUMEngine will not start until received
       * the configuration message.
       */
      auto llcCntrl =
          AbstractStreamAwareController::getController(dstMachineId);
      auto llcSE = llcCntrl->getLLCStreamEngine();
      llcSE->getPUMEngine()->configure(this, args.commands);
    }
  }

  Cycles latency(1); // Just use 1 cycle latency here.

  MLCSE_DPRINTF("Broadcast PUMConfig.\n");

  mlcSE->requestToLLCMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(latency));
}

void MLCPUMManager::compileDataMove(Args &args,
                                    const CacheStreamConfigureDataPtr &config) {

  auto S = config->stream;
  const auto &patternInfo = args.patternInfo.at(S);
  auto myTile = patternInfo.pumTile;

  MLC_S_DPRINTF(config->dynamicId, "[PUM] DataMove Tile %s.\n", myTile);
  DataMoveCompiler compiler(PUMHWConfiguration::getPUMHWConfig(), myTile);

  for (const auto &dep : config->depEdges) {
    assert(dep.type == CacheStreamConfigureData::DepEdge::Type::SendTo);

    auto recvConfig = dep.data;
    auto recvS = recvConfig->stream;
    const auto &recvPatternInfo = args.patternInfo.at(recvS);
    auto recvTile = recvPatternInfo.pumTile;

    if (recvTile != myTile) {
      // TODO: Handle different mapping of source and dest stream.
      MLC_S_PANIC_NO_DUMP(config->dynamicId, "[PUM] Different Tile.");
    }

    if (recvPatternInfo.atomicPatterns.size() != 1) {
      MLC_S_PANIC_NO_DUMP(recvConfig->dynamicId, "[PUM] Multi Recv.");
    }
    const auto &recvPattern = recvPatternInfo.atomicPatterns.front();
    MLC_S_DPRINTF(recvConfig->dynamicId, "[PUM] RecvPattern %s.\n",
                  recvPattern);

    for (const auto &myPattern : patternInfo.atomicPatterns) {
      MLC_S_DPRINTF(config->dynamicId, "[PUM] DataMove Pattern %s ->.\n",
                    myPattern);

      auto commands = compiler.compile(myPattern, recvPattern);
      // Generate the meta information.
      for (auto &cmd : commands) {
        cmd.wordline_bits = patternInfo.scalarElemSize;
        cmd.dynStreamId = config->dynamicId;
        cmd.srcRegion = patternInfo.regionName;
        cmd.srcAccessPattern = myPattern;
        cmd.srcMapPattern = myTile;
        cmd.dstRegion = recvPatternInfo.regionName;
        cmd.dstAccessPattern = recvPattern;
        cmd.dstMapPattern = myTile;
      }
      if (Debug::StreamPUM) {
        for (const auto &command : commands) {
          MLC_S_DPRINTF(config->dynamicId, "%s", command);
        }
      }
      args.commands.insert(args.commands.end(), commands.begin(),
                           commands.end());
    }
  }
}

AffinePatternVecT MLCPUMManager::decoalesceAndDevectorizePattern(
    const CacheStreamConfigureDataPtr &config, const AffinePattern &pattern,
    int scalarElemSize) {
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

void MLCPUMManager::reachSync(int sentPackets) {
  assert(this->context.isActive() && "No Active PUM.");
  this->context.totalAckBanks++;
  this->context.totalSentPackets += sentPackets;
  this->checkDone();
}

void MLCPUMManager::receivePacket() {
  assert(this->context.isActive() && "No Active PUM.");
  this->context.totalRecvPackets++;
  this->checkDone();
}

void MLCPUMManager::checkDone() {

  assert(this->context.isActive() && "No Active PUM.");

  if (this->context.done) {
    return;
  }

  if (this->context.totalAckBanks == this->context.configuredBanks &&
      this->context.totalSentPackets == this->context.totalRecvPackets) {
    MLCSE_DPRINTF("Done! Ack all elements at core.\n");
    for (const auto &config : *context.configs) {
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

    // We are done.
    this->context.done = true;
  }
}

bool MLCPUMManager::receiveStreamEnd(PacketPtr pkt) {
  if (!this->context.isActive()) {
    return false;
  }

  assert(this->context.done && "PUM end before done.");
  MLCSE_DPRINTF("Release PUM context.\n");
  this->context.clear();
  return true;
}