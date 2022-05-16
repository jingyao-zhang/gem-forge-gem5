#include "MLCStrandManager.hh"
#include "LLCStreamEngine.hh"

#include "mem/ruby/protocol/RequestMsg.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStrandSplit.hh"
#include "debug/MLCRubyStreamBase.hh"
#include "debug/MLCRubyStreamLife.hh"

#define DEBUG_TYPE MLCRubyStreamBase
#include "../stream_log.hh"

#define MLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(MLCRubyStream, "[MLC_SE%d]: " format,                                \
          this->controller->getMachineID().num, ##args)

MLCStrandManager::MLCStrandManager(MLCStreamEngine *_mlcSE)
    : mlcSE(_mlcSE), controller(_mlcSE->controller) {}

MLCStrandManager::~MLCStrandManager() {
  for (auto &idStream : this->strandMap) {
    delete idStream.second;
    idStream.second = nullptr;
  }
  this->strandMap.clear();
}

void MLCStrandManager::receiveStreamConfigure(ConfigVec *configs,
                                              MasterID masterId) {

  // auto configs = *(pkt->getPtr<ConfigVec *>());

  this->checkShouldBeSliced(*configs);

  StrandSplitContext splitContext;
  // So far we always split into 64 strands.
  splitContext.totalStrands = 64;
  if (this->canSplitIntoStrands(splitContext, *configs)) {
    this->splitIntoStrands(splitContext, *configs);
  }

  mlcSE->computeReuseInformation(*configs);
  for (auto config : *configs) {
    // this->configureStream(config, pkt->req->masterId());
    this->configureStream(config, masterId);
  }

  // We initalize all LLCDynStreams here (see LLCDynStream.hh)
  LLCDynStream::allocateLLCStreams(this->controller, *configs);

  // Release the configure vec.
  delete configs;
  // delete pkt;
}

void MLCStrandManager::checkShouldBeSliced(ConfigVec &configs) const {

  auto innerMostLoopLevel = 0u;
  for (const auto &config : configs) {
    innerMostLoopLevel =
        std::max(config->stream->getLoopLevel(), innerMostLoopLevel);
  }

  for (auto &config : configs) {
    if (config->storeCallback &&
        config->stream->getLoopLevel() < innerMostLoopLevel) {
      MLC_S_DPRINTF(config->dynamicId,
                    "Disabled Slicing as StoreCallback in LoopLevel %u < "
                    "InnerMostLoopLevel %u.\n",
                    config->stream->getLoopLevel(), innerMostLoopLevel);
      config->shouldBeSlicedToCacheLines = false;
    }
  }
}

bool MLCStrandManager::canSplitIntoStrands(StrandSplitContext &context,
                                           const ConfigVec &configs) const {
  if (!this->controller->myParams->enable_stream_strand) {
    return false;
  }

  /**
   * @brief First we collect NoSplitOuterTripCount hints and check conflicts.
   */
  context.noSplitOuterTripCount = 0;
  for (const auto &config : configs) {
    if (config->hintNoStrandSplitOuterTripCount == 0) {
      continue;
    }
    if (context.noSplitOuterTripCount !=
        config->hintNoStrandSplitOuterTripCount) {
      if (context.noSplitOuterTripCount != 0) {
        MLC_S_DPRINTF(config->dynamicId,
                      "[NoSplit] Conflict NoSplitOuterTrip %ld != Prev %ld.\n",
                      config->hintNoStrandSplitOuterTripCount,
                      context.noSplitOuterTripCount);
        return false;
      }
      MLC_S_DPRINTF(config->dynamicId,
                    "[Strand] Found NoSplitOuterTripCount Hint %ld.\n",
                    config->hintNoStrandSplitOuterTripCount);
      context.noSplitOuterTripCount = config->hintNoStrandSplitOuterTripCount;
    }
  }

  for (const auto &config : configs) {
    if (!this->canSplitIntoStrands(context, config)) {
      return false;
    }
  }
  return true;
}

bool MLCStrandManager::canSplitIntoStrands(StrandSplitContext &context,
                                           ConfigPtr config) const {
  /**
   * We can split streams into strands iff.
   * 1. With known trip count (no StreamLoopBound).
   * 2. Float plan is pure the LLC or Mem.
   * 3. Must be LinearAddrGen (i.e. No PtrChase).
   * 4. Check that all stream can split with Non-Zero NoSplitOuterTripCount.
   * Specifically:
   *
   *   a. TotalTripCount >= NoSplitTripCount and
   *      TotalTripCount % NoSplitTripCount == 0
   *   b. If not simple 1D streams,
   *      need to have outer TripCount align to NoSplitTripCount.
   *   c. If has ReduceStream,
   *      InnerTripCount < TotalTripCount / NoSplitTripCount,
   *      as we currently can not split ReduceStream at InnerMostLoopLevel.
   * TODO: Support split ReduceStream at InnerMostLoop.
   */

  // Initialize more fields.
  auto &perStreamContext =
      context.perStreamContext
          .emplace(std::piecewise_construct,
                   std::forward_as_tuple(config->dynamicId),
                   std::forward_as_tuple())
          .first->second;
  const auto memChannelInterleave = 4096;
  const auto llcBankInterleave = 1024;
  if (config->floatPlan.isFloatedToMem()) {
    // We assume MemCtrl interleavs at 4kB -> 64 cache lines.
    perStreamContext.splitInterleave =
        memChannelInterleave / RubySystem::getBlockSizeBytes();
  } else {
    // We assume LLC interleavs at 1kB -> 16 cache lines.
    perStreamContext.splitInterleave =
        llcBankInterleave / RubySystem::getBlockSizeBytes();
  }

  // 1.
  auto noSplitOuterTrip = context.noSplitOuterTripCount;
  auto splitCount = context.totalStrands;
  if (!config->hasTotalTripCount()) {
    DYN_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                   "[NoSplit] No TripCount.\n");
    return false;
  }
  if (config->getTotalTripCount() < 256) {
    DYN_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                   "[NoSplit] Short TripCount %ld.\n",
                   config->getTotalTripCount());
    return false;
  }
  // 2.
  if (config->floatPlan.isMixedFloat()) {
    DYN_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                   "[NoSplit] Mixed Float.\n");
    return false;
  }
  if (config->floatPlan.getFirstFloatElementIdx() != 0) {
    DYN_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                   "[NoSplit] Delayed Float.\n");
    return false;
  }
  // 3.
  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(config->addrGenCallback);
  if (!linearAddrGen) {
    DYN_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                   "[NoSplit] Not LinearAddrGen.\n");
    return false;
  }

  if (noSplitOuterTrip == 0) {
    if (linearAddrGen->isContinuous(config->addrGenFormalParams,
                                    config->elementSize)) {
      // We can split continuous streams.
      return true;
    }
    /**
     * If the stream is not continous, and we don't have noSplitOuterTripCount,
     * we simply mark noSplitOuterTripCount to 1 so that we are free to split
     * the outer-most dimension.
     */
    noSplitOuterTrip = 1;
    DYN_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                   "[Split] Override NoSplitOuterTripCount to 1.\n");
  }

  // 4.a.
  auto totalTrip = config->getTotalTripCount();
  assert(totalTrip != 0);
  if (totalTrip < noSplitOuterTrip || (totalTrip % noSplitOuterTrip) != 0) {
    DYN_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                   "[NoSplit] TotalTripCount %ld Imcompatible with "
                   "NoSplitTripCount %ld.\n",
                   totalTrip, noSplitOuterTrip);
    return false;
  }

  // 4.b.
  DYN_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                 "[Strand] Analyzing Pattern %s.\n",
                 printAffinePatternParams(config->addrGenFormalParams));
  std::vector<uint64_t> trips;
  std::vector<int64_t> strides;
  assert((config->addrGenFormalParams.size() % 2) == 1);
  uint64_t prevTrip = 1;
  for (int i = 1; i + 1 < config->addrGenFormalParams.size(); i += 2) {
    const auto &p = config->addrGenFormalParams.at(i);
    assert(p.isInvariant);
    auto trip = p.invariant.uint64();
    trips.push_back(trip / prevTrip);
    prevTrip = trip;
    const auto &s = config->addrGenFormalParams.at(i - 1);
    assert(s.isInvariant);
    auto stride = s.invariant.uint64();
    strides.push_back(stride);
  }
  assert(!trips.empty());
  if (trips.size() > 1) {
    // config is not just one dimension.
    int splitDim = trips.size() - 1;
    int64_t outerTripCount = 1;
    while (splitDim >= 0) {
      if (outerTripCount == noSplitOuterTrip) {
        break;
      }
      outerTripCount *= trips.at(splitDim);
      splitDim--;
    }
    if (splitDim < 0) {
      DYN_S_DPRINTF_(
          MLCRubyStrandSplit, config->dynamicId,
          "[NoSplit] NegSplitDim %d Imcompatible with NoSplitTripCount %ld.\n",
          splitDim, noSplitOuterTrip);
      return false;
    }
    /**
     * Notice that here we handle the case when SplitDimTrip % SplitCount != 0.
     * For example, SplitDimTrip = 510, SplitCount = 64.
     * Each strand will handle 8, except the last strand handling only 6.
     */
    auto splitDimTrip = trips.at(splitDim);
    auto splitDimTripPerStrand = (splitDimTrip + splitCount - 1) / splitCount;
    auto innerTrip = totalTrip / noSplitOuterTrip / splitDimTrip;
    assert(innerTrip > 0);

    /**
     * We want to avoid a pathological case when all streams starts at the
     * same bank. This is the case when (splitDimStride * splitDimTripPerStrand)
     * is a multiple of BankInterleave * NumBanks.
     *
     * When this is the case, we try to reduce splitDimTripPerStrand by number
     * of Bank rows.
     */
    auto splitDimStride = strides.at(splitDim);
    auto bankRows = StreamNUCAMap::getNumRows();
    auto bankCols = StreamNUCAMap::getNumCols();
    auto totalBankIntrlv = llcBankInterleave * bankRows * bankCols;
    while ((splitDimStride * splitDimTripPerStrand) % totalBankIntrlv == 0) {
      auto multiple =
          (splitDimStride * splitDimTripPerStrand) / totalBankIntrlv;
      if (multiple > 1) {
        if (splitDimTripPerStrand >= 2) {
          splitDimTripPerStrand /= 2;
        } else {
          break;
        }
      } else {
        if (splitDimTripPerStrand % bankRows == 0) {
          splitDimTripPerStrand /= bankRows;
        } else {
          break;
        }
      }
    }

    perStreamContext.splitDim = splitDim;
    perStreamContext.splitInterleave = innerTrip * splitDimTripPerStrand;
  } else {
    // This is simple linear stream.
    perStreamContext.splitDim = 0;
    perStreamContext.splitInterleave =
        totalTrip / noSplitOuterTrip / splitCount;
  }

  // 4.c
  for (const auto &dep : config->depEdges) {
    if (dep.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {
      auto depS = dep.data->stream;
      if (depS->isPointerChaseIndVar()) {
        DYN_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                       "[Strand] Has PtrChase %s.\n", dep.data->dynamicId);
        return false;
      }
      if (depS->isReduction()) {
        if (!config->hasInnerTripCount() ||
            config->getInnerTripCount() >= totalTrip / noSplitOuterTrip) {
          DYN_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                         "[Strand] CanNot Split Reduce at InnerMostTrip %ld "
                         "TotalTrip %ld NoSplitTrip %ld.\n",
                         config->getInnerTripCount(), totalTrip,
                         noSplitOuterTrip);
          return false;
        }
      }
    }
  }

  /**
   * 4.d If we have PUMElemPerSync set, we need to make sure that:
   *   SplitInterleave * TotalStrands <= PUMElemPerSync
   *   PUMElemPerSync % (SplitInterleave * TotalStrands) == 0
   * And later we need to set the PUMElemPerSync to PUMElemPerSynce /
   * TotalStrands.
   */
  if (config->pumElemPerSync > 0) {
    auto totalInterleave =
        perStreamContext.splitInterleave * context.totalStrands;
    if (config->pumElemPerSync < totalInterleave) {
      DYN_S_DPRINTF_(
          MLCRubyStrandSplit, config->dynamicId,
          "[Strand] NoSplit PUMElemPerSync %ld < Intrlv %d * Strands %d.\n",
          config->pumElemPerSync, perStreamContext.splitInterleave,
          context.totalStrands);
      return false;
    }
    if ((config->pumElemPerSync % totalInterleave) != 0) {
      DYN_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                     "[Strand] NoSplit PUMElemPerSync %ld %% (%d * %d) != 0.\n",
                     config->pumElemPerSync, perStreamContext.splitInterleave,
                     context.totalStrands);
      return false;
    }
  }

  return true;
}

void MLCStrandManager::splitIntoStrands(StrandSplitContext &context,
                                        ConfigVec &configs) {
  // Make a copy of the orginal stream configs.
  auto streamConfigs = configs;
  configs.clear();

  // Split and insert into configs.
  for (auto &config : streamConfigs) {
    auto strandConfigs = this->splitIntoStrands(context, config);
    configs.insert(configs.end(), strandConfigs.begin(), strandConfigs.end());
  }
}

MLCStrandManager::ConfigVec
MLCStrandManager::splitIntoStrands(StrandSplitContext &context,
                                   ConfigPtr config) {
  assert(config->totalStrands == 1 && "Already splited.");
  assert(config->strandIdx == 0 && "Already splited.");
  assert(config->strandSplit.totalStrands == 1 && "Already splited.");
  assert(config->streamConfig == nullptr && "This is a strand.");
  assert(config->isPseudoOffload == false && "Split PseudoOffload.");
  assert(config->rangeSync == false && "Split RangeSync.");
  assert(config->rangeCommit == false && "Split RangeCommit.");
  assert(config->hasBeenCuttedByMLC == false && "Split MLC cut.");
  assert(config->isPointerChase == false && "Split pointer chase.");

  // For now just split by interleave = 1kB / 64B = 16, totalStrands = 64.
  auto &perStreamState = context.perStreamContext.at(config->dynamicId);
  auto initOffset = 0;
  auto interleave = perStreamState.splitInterleave;

  bool isDirect = true;
  StrandSplitInfo strandSplit(initOffset, interleave, context.totalStrands);
  return this->splitIntoStrandsImpl(context, config, strandSplit, isDirect);
}

MLCStrandManager::ConfigVec MLCStrandManager::splitIntoStrandsImpl(
    StrandSplitContext &context, ConfigPtr config, StrandSplitInfo &strandSplit,
    bool isDirect) {

  config->strandSplit = strandSplit;
  config->totalStrands = strandSplit.totalStrands;

  CacheStreamConfigureVec strands;

  if (isDirect) {
    MLC_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                   "[StrandSplit] ---------- Start Split Direct. Original "
                   "AddrPattern %s.\n",
                   printAffinePatternParams(config->addrGenFormalParams));
  } else {
    MLC_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                   "[StrandSplit] ---------- Start Split Indirect.\n");
  }

  for (auto strandIdx = 0; strandIdx < strandSplit.totalStrands; ++strandIdx) {

    // Shallow copy every thing.
    auto strand = std::make_shared<CacheStreamConfigureData>(*config);
    strands.emplace_back(strand);

    /***************************************************************************
     * Properly set the splited fields.
     ***************************************************************************/

    // Strand specific field.
    strand->strandIdx = strandIdx;
    strand->totalStrands = strandSplit.totalStrands;
    strand->strandSplit = strandSplit;
    strand->streamConfig = config;

    /**********************************************************************
     * Don't forget to adjust PUMElemPerSync.
     **********************************************************************/
    if (config->pumElemPerSync > 0) {
      assert((config->pumElemPerSync % context.totalStrands) == 0);
      strand->pumElemPerSync = config->pumElemPerSync / context.totalStrands;
      strand->waitPUMRoundStart = config->waitPUMRoundStart;
    }

    /**********************************************************************
     * Split the address generation only for direct stream.
     **********************************************************************/
    if (isDirect) {
      auto strandAddrGenFormalParams =
          this->splitAffinePattern(context, config, strandSplit, strandIdx);

      MLC_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                     "[StrandSplit] StrandIdx %d AddrPattern %s.\n", strandIdx,
                     printAffinePatternParams(strandAddrGenFormalParams));

      strand->addrGenFormalParams = strandAddrGenFormalParams;
      strand->totalTripCount = strandSplit.getStrandTripCount(
          config->getTotalTripCount(), strandIdx);
      strand->initVAddr = makeLineAddress(
          config->addrGenCallback
              ->genAddr(0, strandAddrGenFormalParams, getStreamValueFail)
              .front());
      if (config->stream->getCPUDelegator()->translateVAddrOracle(
              strand->initVAddr, strand->initPAddr)) {
        strand->initPAddrValid = true;
      } else {
        DynStrandId strandId(config->dynamicId, strandIdx,
                             strandSplit.totalStrands);
        panic("%s: Strand InitVAddr %#x faulted.", strandId, strand->initVAddr);
      }
    }

    // Clear all the edges for now.
    strand->depEdges.clear();
    for (auto &dep : config->depEdges) {
      if (dep.type == CacheStreamConfigureData::DepEdge::Type::SendTo) {
        strand->addSendTo(dep.data, dep.reuse, dep.skip);
      }
      if (dep.type == CacheStreamConfigureData::DepEdge::Type::PUMSendTo) {
        strand->addPUMSendTo(dep.data, dep.broadcastPat, dep.recvPat,
                             dep.recvTile);
      }
      // UsedBy dependence will also be splitted and connected later.
    }
    strand->baseEdges.clear();
    for (auto &base : config->baseEdges) {
      auto baseConfig = base.data.lock();
      assert(baseConfig && "BaseConfig already released?");
      bool isUsedBy = false;
      for (const auto &dep : baseConfig->depEdges) {
        if (dep.data == config &&
            dep.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {
          isUsedBy = true;
          break;
        }
      }
      if (!isUsedBy) {
        // UsedBy is handled below.
        strand->addBaseOn(baseConfig, base.reuse, base.skip);
      }
    }
  }

  /**
   * For all the SendTo relationships, the strand remembers the original
   * StreamConfig. This is already handled above.
   *
   * For all UsedBy dependence, we also split the indirect streams and make
   * the strand point to each other directly. This should saves us the pain of
   * converting StreamElemIdx and StrandElemIdx when communicating between
   * Indirect and Direct streams.
   */
  for (auto &dep : config->depEdges) {
    if (dep.type != CacheStreamConfigureData::DepEdge::Type::UsedBy) {
      continue;
    }
    bool isDirect = false;
    auto depStrands =
        this->splitIntoStrandsImpl(context, dep.data, strandSplit, isDirect);
    assert(depStrands.size() == strands.size());
    for (int strandIdx = 0; strandIdx < depStrands.size(); ++strandIdx) {
      auto strand = strands.at(strandIdx);
      auto depStrand = depStrands.at(strandIdx);
      strand->addUsedBy(depStrand);
      depStrand->totalTripCount = strand->getTotalTripCount();
    }
  }

  return strands;
}

DynStreamFormalParamV MLCStrandManager::splitAffinePattern(
    StrandSplitContext &context, ConfigPtr config,
    const StrandSplitInfo &strandSplit, int strandIdx) {

  auto iter = context.perStreamContext.find(config->dynamicId);

  if (config->addrGenFormalParams.size() > 3) {
    assert(iter != context.perStreamContext.end());

    const auto &psc = iter->second;
    return config->splitAffinePatternAtDim(psc.splitDim, psc.splitInterleave,
                                           strandIdx, strandSplit.totalStrands);

  } else {
    return config->splitLinearParam1D(strandSplit, strandIdx);
  }
}

void MLCStrandManager::configureStream(ConfigPtr config, MasterID masterId) {
  MLC_S_DPRINTF_(MLCRubyStreamLife,
                 DynStrandId(config->dynamicId, config->strandIdx),
                 "[Strand] Received StreamConfig, TotalTripCount %lu.\n",
                 config->totalTripCount);
  /**
   * Do not release the pkt and streamConfigureData as they should be
   * forwarded to the LLC bank and released there. However, we do need to fix
   * up the initPAddr to our LLC bank if case it is not valid. ! This has to
   * be done before initializing the MLCDynStream so that it ! knows the
   * initial llc bank.
   */
  if (!config->initPAddrValid) {
    config->initPAddr = this->controller->getAddressToOurLLC();
    config->initPAddrValid = true;
  }

  /**
   * ! We initialize the indirect stream first so that
   * ! the direct stream's constructor can start notify it about base stream
   * data.
   */
  std::vector<MLCDynIndirectStream *> indirectStreams;
  for (const auto &edge : config->depEdges) {
    if (edge.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {
      const auto &indirectStreamConfig = edge.data;
      // Let's create an indirect stream.
      auto indirectStream = new MLCDynIndirectStream(
          indirectStreamConfig, this->controller,
          mlcSE->responseToUpperMsgBuffer, mlcSE->requestToLLCMsgBuffer,
          config->dynamicId /* Root dynamic stream id. */);
      this->strandMap.emplace(indirectStream->getDynStrandId(), indirectStream);
      indirectStreams.push_back(indirectStream);

      for (const auto &ISDepEdge : indirectStreamConfig->depEdges) {
        if (ISDepEdge.type != CacheStreamConfigureData::DepEdge::UsedBy) {
          continue;
        }
        /**
         * So far we don't support Two-Level Indirect LLCStream, except:
         * 1. IndirectRedcutionStream.
         * 2. Two-Level IndirectStoreComputeStream.
         */
        auto ISDepS = ISDepEdge.data->stream;
        if (ISDepS->isReduction() || ISDepS->isStoreComputeStream()) {
          auto IIS = new MLCDynIndirectStream(
              ISDepEdge.data, this->controller, mlcSE->responseToUpperMsgBuffer,
              mlcSE->requestToLLCMsgBuffer,
              config->dynamicId /* Root dynamic stream id. */);
          this->strandMap.emplace(IIS->getDynStrandId(), IIS);

          indirectStreams.push_back(IIS);
          continue;
        }
        panic("Two-Level Indirect LLCStream is not supported: %s.",
              ISDepEdge.data->dynamicId);
      }
    }
  }
  // Create the direct stream.
  auto directStream = new MLCDynDirectStream(
      config, this->controller, mlcSE->responseToUpperMsgBuffer,
      mlcSE->requestToLLCMsgBuffer, indirectStreams);
  this->strandMap.emplace(directStream->getDynStrandId(), directStream);

  /**
   * If there is reuse for this stream, we cut the stream's totalTripCount.
   * ! This can only be done after initializing MLC streams, as only LLC
   * streams ! should be cut.
   */
  {
    auto reuseIter = mlcSE->reverseReuseInfoMap.find(config->dynamicId);
    if (reuseIter != mlcSE->reverseReuseInfoMap.end()) {
      auto cutElementIdx = reuseIter->second.targetCutElementIdx;
      auto cutLineVAddr = reuseIter->second.targetCutLineVAddr;
      if (config->totalTripCount == -1 ||
          config->totalTripCount > cutElementIdx) {
        config->totalTripCount = cutElementIdx;
        config->hasBeenCuttedByMLC = true;
        directStream->setLLCCutLineVAddr(cutLineVAddr);
        assert(config->depEdges.empty() &&
               "Reuse stream with indirect stream is not supported.");
      }
    }
  }

  // Configure Remote SE.
  this->sendConfigToRemoteSE(config, masterId);
}

void MLCStrandManager::sendConfigToRemoteSE(ConfigPtr streamConfigureData,
                                            MasterID masterId) {

  /**
   * Set the RemoteSE to LLC SE or Mem SE, depending on the FloatPlan on the
   * FirstFloatElemIdx.
   */
  auto firstFloatElemIdx =
      streamConfigureData->floatPlan.getFirstFloatElementIdx();
  auto firstFloatElemMachineTypee =
      streamConfigureData->floatPlan.getMachineTypeAtElem(firstFloatElemIdx);

  auto initPAddrLine = makeLineAddress(streamConfigureData->initPAddr);
  auto remoteSEMachineID = this->controller->mapAddressToLLCOrMem(
      initPAddrLine, firstFloatElemMachineTypee);

  // Create a new packet.
  RequestPtr req = std::make_shared<Request>(
      streamConfigureData->initPAddr, sizeof(streamConfigureData), 0, masterId);
  PacketPtr pkt = new Packet(req, MemCmd::StreamConfigReq);
  uint8_t *pktData =
      reinterpret_cast<uint8_t *>(new ConfigPtr(streamConfigureData));
  pkt->dataDynamic(pktData);
  // Enqueue a configure packet to the target LLC bank.
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = initPAddrLine;
  msg->m_Type = CoherenceRequestType_STREAM_CONFIG;
  msg->m_Requestors.add(this->controller->getMachineID());
  msg->m_Destination.add(remoteSEMachineID);
  msg->m_pkt = pkt;

  /**
   * If we enable PartialConfig, we assume the static parameters are
   * already configured at RemoteSE, and thus we only need to send out
   * dynamic parameters. Here we assume it can be represented as a
   * control message.
   */

  if (this->controller->myParams->enable_stream_partial_config) {
    msg->m_MessageSize = MessageSizeType_Control;
  } else {
    msg->m_MessageSize = MessageSizeType_Data;
  }

  Cycles latency(1); // Just use 1 cycle latency here.

  MLC_S_DPRINTF(streamConfigureData->dynamicId,
                "Send Config to RemoteSE at %s.\n", remoteSEMachineID);

  mlcSE->requestToLLCMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(latency));
}

void MLCStrandManager::receiveStreamEnd(const std::vector<DynStreamId> &endIds,
                                        MasterID masterId) {
  for (const auto &endId : endIds) {
    this->endStream(endId, masterId);
  }
}

void MLCStrandManager::endStream(const DynStreamId &endId, MasterID masterId) {
  MLC_S_DPRINTF_(MLCRubyStreamLife, endId, "Received StreamEnd.\n");

  /**
   * Find all root strands and record the PAddr and MachineType to multicast
   * the StreamEnd message.
   */
  std::vector<std::pair<DynStrandId, std::pair<Addr, MachineType>>>
      rootStrandTailPAddrMachineTypeVec;
  for (const auto &entry : this->strandMap) {
    const auto &strandId = entry.first;
    if (strandId.dynStreamId == endId) {
      auto dynS = entry.second;
      rootStrandTailPAddrMachineTypeVec.emplace_back(
          strandId, dynS->getRemoteTailPAddrAndMachineType());
    }
  }
  assert(!rootStrandTailPAddrMachineTypeVec.empty() &&
         "Failed to find the ending root stream.");

  // End all streams with the correct root stream id (indirect streams).
  for (auto streamIter = this->strandMap.begin(),
            streamEnd = this->strandMap.end();
       streamIter != streamEnd;) {
    auto stream = streamIter->second;
    if (stream->getRootDynStreamId() == endId) {
      /**
       * ? Can we release right now?
       * We need to make sure all the seen request is responded (with dummy
       * data).
       * TODO: In the future, if the core doesn't require to send the request,
       * TODO: we are fine to simply release the stream.
       */
      mlcSE->endedStreamDynamicIds.insert(stream->getDynStreamId());
      stream->endStream();
      delete stream;
      streamIter->second = nullptr;
      streamIter = this->strandMap.erase(streamIter);
    } else {
      ++streamIter;
    }
  }

  // Clear the reuse information.
  if (mlcSE->reuseInfoMap.count(endId)) {
    mlcSE->reverseReuseInfoMap.erase(
        mlcSE->reuseInfoMap.at(endId).targetStreamId);
    mlcSE->reuseInfoMap.erase(endId);
  }

  // For each remote root strand, send out a StreamEnd packet.
  for (const auto &entry : rootStrandTailPAddrMachineTypeVec) {

    const auto &strandId = entry.first;
    auto rootLLCStreamPAddr = entry.second.first;
    auto rootStreamOffloadedMachineType = entry.second.second;

    auto rootLLCStreamPAddrLine = makeLineAddress(rootLLCStreamPAddr);
    auto rootStreamOffloadedBank = this->controller->mapAddressToLLCOrMem(
        rootLLCStreamPAddrLine, rootStreamOffloadedMachineType);
    auto copyStrandId = new DynStrandId(strandId);
    RequestPtr req = std::make_shared<Request>(
        rootLLCStreamPAddrLine, sizeof(copyStrandId), 0, masterId);
    PacketPtr pkt = new Packet(req, MemCmd::StreamEndReq);
    uint8_t *pktData = new uint8_t[req->getSize()];
    *(reinterpret_cast<uint64_t *>(pktData)) =
        reinterpret_cast<uint64_t>(copyStrandId);
    pkt->dataDynamic(pktData);

    if (this->controller->myParams->enable_stream_idea_end) {
      auto remoteController =
          AbstractStreamAwareController::getController(rootStreamOffloadedBank);
      auto remoteSE = remoteController->getLLCStreamEngine();
      // StreamAck is also disguised as StreamData.
      remoteSE->receiveStreamEnd(pkt);
      MLC_S_DPRINTF(strandId, "Send ideal StreamEnd to %s.\n",
                    rootStreamOffloadedBank);

    } else {
      // Enqueue a end packet to the target LLC bank.
      auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
      msg->m_addr = rootLLCStreamPAddrLine;
      msg->m_Type = CoherenceRequestType_STREAM_END;
      msg->m_Requestors.add(this->controller->getMachineID());
      msg->m_Destination.add(rootStreamOffloadedBank);
      msg->m_MessageSize = MessageSizeType_Control;
      msg->m_pkt = pkt;

      Cycles latency(1); // Just use 1 cycle latency here.

      mlcSE->requestToLLCMsgBuffer->enqueue(
          msg, this->controller->clockEdge(),
          this->controller->cyclesToTicks(latency));
    }
  }
}

StreamEngine *MLCStrandManager::getCoreSE() const {
  if (!this->strandMap.empty()) {
    return this->strandMap.begin()->second->getStaticStream()->se;
  } else {
    return nullptr;
  }
}

MLCDynStream *MLCStrandManager::getStreamFromStrandId(const DynStrandId &id) {
  auto iter = this->strandMap.find(id);
  if (iter == this->strandMap.end()) {
    return nullptr;
  }
  return iter->second;
}

MLCDynStream *
MLCStrandManager::getStreamFromCoreSliceId(const DynStreamSliceId &sliceId) {
  if (!sliceId.isValid()) {
    return nullptr;
  }
  // TODO: Support the translation.
  auto dynS = this->getStreamFromStrandId(sliceId.getDynStrandId());
  if (dynS) {
    assert(
        dynS->getDynStrandId().totalStrands == 1 &&
        "Translation between CoreSlice and StrandSlice not implemented yet.");
  }
  return dynS;
}

void MLCStrandManager::checkCoreCommitProgress() {
  for (auto &idStream : this->strandMap) {
    auto S = dynamic_cast<MLCDynDirectStream *>(idStream.second);
    if (!S || !S->shouldRangeSync()) {
      continue;
    }
    S->checkCoreCommitProgress();
  }
}

bool MLCStrandManager::isStreamElemAcked(
    const DynStreamId &streamId, uint64_t streamElemIdx,
    MLCDynStream::ElementCallback callback) {

  /**
   * We first get the first Strand. And then get TargetStrandId and
   * StreamElemIdx.
   *
   * NOTE: This does not support InitOffset.
   *
   * Define InterleaveCount = StreamElemIdx / (Interleave * TotalStrands).
   * For all strands:
   * 1. If streandId < targetStrandId:
   *    Check that ((InterleaveCount + 1) * Interleave - 1) is Acked.
   * 2. If strandId == targetStrandId:
   *    Check that StreamElemIdx is Acked.
   * 3. If strandId > targetStrandId and InterleaveCount > 0
   *    Check that IntleaveCount * Interleave is Acked.
   *
   */

  auto firstDynS = this->mlcSE->getStreamFromStrandId(DynStrandId(streamId));
  assert(firstDynS && "MLCDynS already released?");

  auto firstConfig = firstDynS->getConfig();
  const auto &splitInfo = firstConfig->strandSplit;
  assert(splitInfo.initOffset == 0 && "Can not NonZero InitOffset.");

  auto targetStrandId =
      firstConfig->getStrandIdFromStreamElemIdx(streamElemIdx);

  auto interleaveCount =
      streamElemIdx / (splitInfo.interleave * splitInfo.totalStrands);
  for (auto strandIdx = 0; strandIdx < splitInfo.totalStrands; ++strandIdx) {
    auto strandId = DynStrandId(streamId, strandIdx, splitInfo.totalStrands);
    auto dynS = this->mlcSE->getStreamFromStrandId(strandId);
    assert(dynS && "MLCDynS already released.");

    uint64_t checkStrandElemIdx = 0;
    if (strandIdx < targetStrandId.strandIdx) {
      checkStrandElemIdx = (interleaveCount + 1) * splitInfo.interleave - 1;
    } else if (strandIdx > targetStrandId.strandIdx) {
      checkStrandElemIdx = interleaveCount * splitInfo.interleave;
    } else {
      checkStrandElemIdx =
          dynS->getConfig()->getStrandElemIdxFromStreamElemIdx(streamElemIdx);
    }
    if (!dynS->isElementAcked(checkStrandElemIdx)) {
      MLC_S_DPRINTF(dynS->getDynStrandId(),
                    "NoAck for StrandElem %lu TargetStrandIdx %d "
                    "TargetStreamElemIdx %lu.\n",
                    checkStrandElemIdx, targetStrandId.strandIdx,
                    streamElemIdx);
      dynS->registerElementAckCallback(checkStrandElemIdx, callback);
      return false;
    }
  }

  return true;
}
