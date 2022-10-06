#include "MLCStrandManager.hh"
#include "LLCStreamEngine.hh"

#include "../stream_float_policy.hh"

#include "mem/ruby/protocol/RequestMsg.hh"
#include "sim/stream_nuca/stream_nuca_manager.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStrandSplit.hh"
#include "debug/MLCRubyStreamBase.hh"
#include "debug/MLCRubyStreamLife.hh"

#define DEBUG_TYPE MLCRubyStreamBase
#include "../stream_log.hh"

#define STRAND_LOG_(X, dynId, format, args...)                                 \
  {                                                                            \
    DYN_S_DPRINTF_(X, dynId, format, ##args);                                  \
    std::ostringstream s;                                                      \
    ccprintf(s, format, ##args);                                               \
    StreamFloatPolicy::logS(dynId) << s.str() << std::flush;                   \
  }

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
      /**
       * Check if we use any ValueBaseS from InnerLoop.
       */
      bool useInnerLoopNonReduceValueBaseS = false;
      for (const auto &edge : config->baseEdges) {
        auto baseConfig = edge.data.lock();
        assert(baseConfig && "Missing BaseConfig.");
        if (baseConfig->stream->getLoopLevel() >
                config->stream->getLoopLevel() &&
            !baseConfig->stream->isReduction()) {
          useInnerLoopNonReduceValueBaseS = true;
          break;
        }
      }
      if (useInnerLoopNonReduceValueBaseS) {
        MLC_S_DPRINTF(config->dynamicId,
                      "Disabled StoreS Slicing in LoopLevel %u < %u.\n",
                      config->stream->getLoopLevel(), innerMostLoopLevel);
        config->shouldBeSlicedToCacheLines = false;
      }
    }
    /**
     * We also disable slicing if we are sending to inner-loop streams.
     */
    if (config->sendToInnerLoopStream()) {
      MLC_S_DPRINTF(config->dynamicId,
                    "Disabled Slicing as send to InnerLoopS.\n");
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
  context.noSplitOuterTrip = 0;
  for (const auto &config : configs) {
    if (config->hintNoStrandSplitOuterTrip == 0) {
      continue;
    }
    if (context.noSplitOuterTrip != config->hintNoStrandSplitOuterTrip) {
      if (context.noSplitOuterTrip != 0) {
        STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                    "[NoSplit] Conflict NoSplitOuterTrip %ld != Prev %ld.\n",
                    config->hintNoStrandSplitOuterTrip,
                    context.noSplitOuterTrip);
        return false;
      }
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[Strand] Found NoSplitOuterTrip Hint %ld.\n",
                  config->hintNoStrandSplitOuterTrip);
      context.noSplitOuterTrip = config->hintNoStrandSplitOuterTrip;
    }
  }

  for (const auto &config : configs) {
    if (!this->precheckSplitable(context, config)) {
      return false;
    }
  }

  for (const auto &config : configs) {
    if (!this->chooseSplitDimIntrlv(context, config)) {
      return false;
    }
  }

  // Some additional check.
  if (!this->fixSplitDimIntrlv(context, configs)) {
    return false;
  }

  for (const auto &config : configs) {
    if (!this->postcheckSplitable(context, config)) {
      return false;
    }
  }
  return true;
}

bool MLCStrandManager::precheckSplitable(StrandSplitContext &context,
                                         ConfigPtr config) const {
  /**
   * All the check that does not require SplitDim known.
   * 1. With known trip count (no StreamLoopBound).
   * 2. Float plan is pure the LLC or Mem.
   * 3. Must be LinearAddrGen (i.e. No PtrChase).
   */

  // Initialize more fields.
  auto &perStreamContext =
      context.perStreamContext
          .emplace(std::piecewise_construct,
                   std::forward_as_tuple(config->dynamicId),
                   std::forward_as_tuple())
          .first->second;
  const auto memChannelIntrlv = 4096;
  const auto llcBankIntrlv = 1024;
  if (config->floatPlan.isFloatedToMem()) {
    // We assume MemCtrl interleavs at 4kB -> 64 cache lines.
    perStreamContext.splitTripPerStrand =
        memChannelIntrlv / RubySystem::getBlockSizeBytes();
  } else {
    // We assume LLC interleavs at 1kB -> 16 cache lines.
    perStreamContext.splitTripPerStrand =
        llcBankIntrlv / RubySystem::getBlockSizeBytes();
  }

  // 1.
  if (!config->hasTotalTripCount()) {
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "[NoSplit] No TripCount.\n");
    return false;
  }
  if (config->getTotalTripCount() < 128) {
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "[NoSplit] Short TripCount %ld.\n",
                config->getTotalTripCount());
    return false;
  }
  // 2.
  if (config->floatPlan.isMixedFloat()) {
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "[NoSplit] Mixed Float.\n");
    return false;
  }
  if (config->floatPlan.getFirstFloatElementIdx() != 0) {
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "[NoSplit] Delayed Float.\n");
    return false;
  }
  // 3.
  if (!std::dynamic_pointer_cast<LinearAddrGenCallback>(
          config->addrGenCallback)) {
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "[NoSplit] Not LinearAddrGen.\n");
    return false;
  }

  for (const auto &dep : config->depEdges) {
    if (dep.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {
      auto depS = dep.data->stream;
      if (depS->isPointerChaseIndVar()) {
        STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                    "[NoSplit] Has PtrChase %s.\n", dep.data->dynamicId);
        return false;
      }
    }
  }

  return true;
}

void MLCStrandManager::tryAvoidStartStrandsAtSameBank(
    ConfigPtr config, const int llcBankIntrlv, const int64_t splitDimStride,
    int64_t &splitDimTripPerStrand) const {

  auto bankRows = StreamNUCAMap::getNumRows();
  auto bankCols = StreamNUCAMap::getNumCols();
  auto llcBanks = bankRows * bankCols;

  auto totalBankIntrlv = llcBankIntrlv * bankRows * bankCols;
  while ((splitDimStride * splitDimTripPerStrand) % totalBankIntrlv == 0) {
    bool updated = false;
    auto multiple = (splitDimStride * splitDimTripPerStrand) / totalBankIntrlv;
    if (multiple > 1) {
      if (splitDimTripPerStrand >= 2) {
        STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                    "[Strand] Adjust SplitDimTrip/Strand %lu -> %lu.\n",
                    splitDimTripPerStrand, splitDimTripPerStrand / 2);
        splitDimTripPerStrand /= 2;
        updated = true;
      }
    } else {
      if (splitDimTripPerStrand >= llcBanks &&
          splitDimTripPerStrand % llcBanks == 0) {
        STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                    "[Strand] SplitToBanks SplitDimTrip/Strand %lu -> %lu.\n",
                    splitDimTripPerStrand, splitDimTripPerStrand / llcBanks);
        splitDimTripPerStrand /= llcBanks;
        updated = true;
      } else if (splitDimTripPerStrand >= bankRows &&
                 splitDimTripPerStrand % bankRows == 0) {
        STRAND_LOG_(
            MLCRubyStrandSplit, config->dynamicId,
            "[Strand] SplitToBankRows SplitDimTrip/Strand %lu -> %lu.\n",
            splitDimTripPerStrand, splitDimTripPerStrand / bankRows);
        splitDimTripPerStrand /= bankRows;
        updated = true;
      }
    }
    if (!updated) {
      break;
    }
  }
}

bool MLCStrandManager::chooseSplitDimIntrlv(StrandSplitContext &context,
                                            ConfigPtr config) const {

  /**
   * Basically pick the SplitDim and Interleave.
   */

  auto &perStreamContext = context.perStreamContext.at(config->dynamicId);

  auto noSplitOuterTrip = context.noSplitOuterTrip;
  auto splitCount = context.totalStrands;

  if (noSplitOuterTrip == 0) {
    /**
     * If the stream is not continous, and we don't have noSplitOuterTripCount,
     * we simply mark noSplitOuterTripCount to 1 so that we are free to split
     * the outer-most dimension.
     */
    noSplitOuterTrip = 1;
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "[Strand] Override NoSplitOuterTrip to 1.\n");
  }

  // 4.a.
  auto totalTrip = config->getTotalTripCount();
  assert(totalTrip != 0);
  if (totalTrip < noSplitOuterTrip || (totalTrip % noSplitOuterTrip) != 0) {
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "[NoSplit] TotalTrip %ld Imcompatible with NoSplitTrip %ld.\n",
                totalTrip, noSplitOuterTrip);
    return false;
  }

  // 4.b.
  STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
              "[Strand] Analyzing Pattern %s.\n",
              printAffinePatternParams(config->addrGenFormalParams));
  auto &trips = perStreamContext.trips;
  auto &strides = perStreamContext.strides;
  {
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
  }

  int splitDim = trips.size() - 1;
  {
    int64_t outerTripCount = 1;
    while (splitDim >= 0) {
      if (outerTripCount == noSplitOuterTrip) {
        break;
      }
      outerTripCount *= trips.at(splitDim);
      splitDim--;
    }
  }
  if (splitDim < 0) {
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "[NoSplit] NegSplitDim %d NoSplitOuterTrip %ld.\n", splitDim,
                noSplitOuterTrip);
    return false;
  }
  /**
   * As a hack here, we try to split at inner level in these conditions:
   * 1. OuterDimTrip < SplitCount. (for 3D stencil)
   * to see if we can find an inner dimension with TripCount >= SplitCount.
   */
  if (splitDim == trips.size() - 1) {
    auto splitTrips = trips.at(splitDim);
    if (splitTrips < splitCount && trips.size() >= 2) {
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[Strand] Override SplitDim to %d.\n", trips.size() - 2);
      splitDim = trips.size() - 2;
    }
  }
  /**
   * Notice that here we handle the case when SplitDimTrip % SplitCount != 0.
   * For example, SplitDimTrip = 510, SplitCount = 64.
   * Each strand will handle 8, except the last strand handling only 6.
   */
  auto &outerTrip = perStreamContext.outerTrip;
  auto &innerTrip = perStreamContext.innerTrip;
  auto splitDimTrip = trips.at(splitDim);
  outerTrip =
      AffinePattern::reduce_mul(trips.begin() + splitDim + 1, trips.end(), 1);
  innerTrip = totalTrip / outerTrip / splitDimTrip;
  assert(innerTrip > 0);

  auto splitDimTripPerStrand = (splitDimTrip + splitCount - 1) / splitCount;

  STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
              "[Strand] SplitDim %d SplitDimTrip %lu SplitCount %d "
              "SplitDimTrip/Strand %lu.\n",
              splitDim, splitDimTrip, splitCount, splitDimTripPerStrand);

  /**
   * We want to avoid a pathological case when all streams starts at the
   * same bank. This is the case when (splitDimStride * splitDimTripPerStrand)
   * is a multiple of BankInterleave * NumBanks.
   *
   * When this is the case, we try to reduce splitDimTripPerStrand by number
   * of Bank rows.
   *
   * NOTE: Do not do this for PUM region, as they are not interleaved in the
   * same way.
   */
  auto splitDimStride = strides.at(splitDim);

  assert(config->initPAddrValid && "InitPAddr is not valid.");
  auto region = StreamNUCAMap::getRangeMapContaining(config->initPAddr);
  if (region) {
    if (!region->isStreamPUM) {
      const auto llcBankIntrlv = region->interleave;
      this->tryAvoidStartStrandsAtSameBank(
          config, llcBankIntrlv, splitDimStride, splitDimTripPerStrand);
    } else {
      // Don't do this on PUM region.
    }
  } else {
    // Default 1kB interleave.
    const auto llcBankIntrlv = 1024;
    this->tryAvoidStartStrandsAtSameBank(config, llcBankIntrlv, splitDimStride,
                                         splitDimTripPerStrand);
  }

  perStreamContext.splitDim = splitDim;
  perStreamContext.splitTripPerStrand = splitDimTripPerStrand;

  return true;
}

bool MLCStrandManager::fixSplitDimIntrlv(StrandSplitContext &context,
                                         const ConfigVec &configs) const {

  /**
   * We check that:
   *
   * 1. All streams have the same Dim - SplitDim.
   * 2. Same SplitDimTrip.
   * 3. If SplitDimIntrlve is different, pick the minimal one.
   *
   */
  if (configs.empty()) {
    return false;
  }

  auto &firstStreamContext =
      context.perStreamContext.at(configs.front()->dynamicId);
  auto firstDims = firstStreamContext.trips.size();
  auto firstSplitDim = firstStreamContext.splitDim;
  auto firstSplitDimTrip = firstStreamContext.trips.at(firstSplitDim);

  auto minSplitDimIntrlv = firstSplitDimTrip;

  for (const auto &config : configs) {
    const auto &perStreamContext =
        context.perStreamContext.at(config->dynamicId);
    auto dims = perStreamContext.trips.size();
    auto splitDim = perStreamContext.splitDim;
    auto splitDimTrip = perStreamContext.trips.at(splitDim);
    if (dims - splitDim != firstDims - firstSplitDim) {
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[Strand] Mismatch in SplitDim %d-%d %d-%d %s.", dims,
                  splitDim, firstDims, firstSplitDim,
                  configs.front()->dynamicId);
      return false;
    }
    if (splitDimTrip != firstSplitDimTrip) {
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[Strand] Mismatch in SplitDimTrip %ld %ld %s.\n",
                  splitDimTrip, firstSplitDimTrip, configs.front()->dynamicId);
      return false;
    }

    auto splitDimIntrlv = perStreamContext.splitTripPerStrand;
    if (splitDimIntrlv < minSplitDimIntrlv) {
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[Strand] Min SplitDimIntrlv %d.\n", splitDimIntrlv);
      minSplitDimIntrlv = splitDimIntrlv;
    }
  }

  for (const auto &config : configs) {
    auto &perStreamContext = context.perStreamContext.at(config->dynamicId);
    auto splitDimIntrlv = perStreamContext.splitTripPerStrand;
    if (splitDimIntrlv > minSplitDimIntrlv) {
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[Strand] Adjust SplitDimIntrlv %d -> %d.\n", splitDimIntrlv,
                  minSplitDimIntrlv);
      perStreamContext.splitTripPerStrand = minSplitDimIntrlv;
    }

    // /**
    //  * Check if we need to handle TailInterleave.
    //  */
    // auto splitDim = perStreamContext.splitDim;
    // auto splitDimTrip = perStreamContext.trips.at(splitDim);
    // auto splitCount = context.totalStrands;
    // auto splitDimTripPerStrand = (splitDimTrip + splitCount - 1) /
    // splitCount; auto innerTrip = perStreamContext.innerTrip; if
    // (config->getTotalTripCount() > innerTrip * splitDimTrip) {
    //   // There are some outer rounds.
    //   auto v = splitDimTripPerStrand * splitCount;
    //   if (v < splitDimTrip) {
    //     if (splitDimTrip % v != 0) {
    //       DYN_S_PANIC(config->dynamicId,
    //                   "[Strand] Can not handle this TailInterleave.\n");
    //     }
    //   } else {
    //     STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
    //                 "[Strand] TailIntreleave %lu * %lu.\n", innerTrip,
    //                 v - splitDimTrip);
    //     perStreamContext.splitTailInterleave = innerTrip * (v -
    //     splitDimTrip);
    //   }
    // }
  }

  return true;
}

bool MLCStrandManager::postcheckSplitable(StrandSplitContext &context,
                                          ConfigPtr config) const {
  /**
   * All the check that requires SplitDim known.
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

  auto &perStreamContext = context.perStreamContext.at(config->dynamicId);
  auto totalTrip = config->getTotalTripCount();
  const auto &outerTrip = perStreamContext.outerTrip;
  // 4.c
  for (const auto &dep : config->depEdges) {
    if (dep.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {
      auto depS = dep.data->stream;
      if (depS->isReduction()) {
        if (!config->hasInnerTripCount() ||
            config->getInnerTripCount() >= totalTrip / outerTrip) {
          STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                      "[Strand] CanNot Split Reduce at InnerMostTrip %ld "
                      "TotalTrip %ld OutTrip %ld.\n",
                      config->getInnerTripCount(), totalTrip, outerTrip);
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
    auto totalInterleave = perStreamContext.splitTripPerStrand *
                           perStreamContext.innerTrip * context.totalStrands;
    if (config->pumElemPerSync < totalInterleave) {
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[Strand] NoSplit PUMElemPerSync %ld < Intrlv %ld * %ld * "
                  "Strands %d.\n",
                  config->pumElemPerSync, perStreamContext.splitTripPerStrand,
                  perStreamContext.innerTrip, context.totalStrands);
      return false;
    }
    if ((config->pumElemPerSync % totalInterleave) != 0) {
      STRAND_LOG_(
          MLCRubyStrandSplit, config->dynamicId,
          "[Strand] NoSplit PUMElemPerSync %ld %% (%d * %ld * %d) != 0.\n",
          config->pumElemPerSync, perStreamContext.splitTripPerStrand,
          perStreamContext.innerTrip, context.totalStrands);
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
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "---------------Split Strands\n");
    for (const auto &strandConfig : strandConfigs) {
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId, "Strand %s %s.\n",
                  DynStrandId(strandConfig->dynamicId, strandConfig->strandIdx,
                              strandConfig->totalStrands),
                  printAffinePatternParams(strandConfig->addrGenFormalParams));
    }
  }
}

MLCStrandManager::ConfigVec
MLCStrandManager::splitIntoStrands(StrandSplitContext &context,
                                   ConfigPtr config) {
  assert(config->totalStrands == 1 && "Already splited.");
  assert(config->strandIdx == 0 && "Already splited.");
  assert(config->strandSplit.getTotalStrands() == 1 && "Already splited.");
  assert(config->streamConfig == nullptr && "This is a strand.");
  assert(config->isPseudoOffload == false && "Split PseudoOffload.");
  assert(config->rangeSync == false && "Split RangeSync.");
  assert(config->rangeCommit == false && "Split RangeCommit.");
  assert(config->hasBeenCuttedByMLC == false && "Split MLC cut.");
  assert(config->isPointerChase == false && "Split pointer chase.");

  // For now just split by interleave = 1kB / 64B = 16, totalStrands = 64.
  auto &psc = context.perStreamContext.at(config->dynamicId);
  // auto interleave =
  //     perStreamState.splitTripPerStrand * perStreamState.innerTrip;
  // auto tailInterleave = perStreamState.splitTailInterleave;

  bool isDirect = true;
  StrandSplitInfo strandSplit(psc.innerTrip, psc.trips.at(psc.splitDim),
                              psc.splitTripPerStrand, context.totalStrands);
  // StrandSplitInfo strandSplit(interleave, tailInterleave,
  // context.totalStrands);
  return this->splitIntoStrandsImpl(context, config, strandSplit, isDirect);
}

MLCStrandManager::ConfigVec MLCStrandManager::splitIntoStrandsImpl(
    StrandSplitContext &context, ConfigPtr config, StrandSplitInfo strandSplit,
    bool isDirect) {

  if (isDirect) {
    MLC_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                   "[StrandSplit] ---------- Start Split Direct. Original "
                   "AddrPattern %s.\n",
                   printAffinePatternParams(config->addrGenFormalParams));
  } else {
    MLC_S_DPRINTF_(MLCRubyStrandSplit, config->dynamicId,
                   "[StrandSplit] ---------- Start Split Indirect.\n");
    /*********************************************************************
     * Now that IndS may have reuse on the BaseS. Adjust the StrandSplit.
     *********************************************************************/
    for (const auto &base : config->baseEdges) {
      if (base.isUsedBy) {
        if (base.reuse > 1) {
          assert(strandSplit.getTailInterleave() == 0 &&
                 "Cannot handle TailInterleave and IndS with reuse for now.");
          strandSplit.setInnerTrip(strandSplit.getInnerTrip() * base.reuse);
          MLC_S_DPRINTF_(
              MLCRubyStrandSplit, config->dynamicId,
              "[StrandSplit] Adjust Interleave by IndReuse %d -> %ld.\n",
              base.reuse, strandSplit.getInterleave());
        }
        break;
      }
    }
  }

  config->strandSplit = strandSplit;
  config->totalStrands = strandSplit.getTotalStrands();

  CacheStreamConfigureVec strands;

  for (auto strandIdx = 0; strandIdx < strandSplit.getTotalStrands();
       ++strandIdx) {

    // Shallow copy every thing.
    auto strand = std::make_shared<CacheStreamConfigureData>(*config);
    strands.emplace_back(strand);

    /***************************************************************************
     * Properly set the splited fields.
     ***************************************************************************/

    // Strand specific field.
    strand->strandIdx = strandIdx;
    strand->totalStrands = strandSplit.getTotalStrands();
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
        strand->initPAddr = 0;
        strand->initPAddrValid = false;
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
      if (base.isUsedBy) {
        // UsedBy is handled below.
        continue;
      }
      if (base.isUsedAffineIV) {
        strand->addBaseAffineIV(baseConfig, base.reuse, base.skip);
      } else {
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
      strand->addUsedBy(depStrand, dep.reuse);
      depStrand->totalTripCount = strand->getTotalTripCount();
    }
  }

  return strands;
}

DynStreamFormalParamV MLCStrandManager::splitAffinePattern(
    StrandSplitContext &context, ConfigPtr config,
    const StrandSplitInfo &strandSplit, int strandIdx) {

  auto iter = context.perStreamContext.find(config->dynamicId);

  assert(iter != context.perStreamContext.end());

  const auto &psc = iter->second;
  return config->splitAffinePatternAtDim(
      psc.splitDim, psc.splitTripPerStrand * psc.innerTrip, strandIdx,
      strandSplit.getTotalStrands());
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
   * Record the strand information.
   */
  if (config->isPUMPrefetch) {
    config->stream->statistic.numPrefetchStrands++;
  } else {
    config->stream->statistic.numStrands++;
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

void MLCStrandManager::tryMarkPUMRegionCached(const DynStreamId &dynId) {

  std::vector<std::pair<DynStrandId, std::pair<Addr, MachineType>>>
      rootStrandTailPAddrMachineTypeVec;

  Stream *S = nullptr;
  AddrGenCallbackPtr addrGenCb;
  DynStreamFormalParamV formalParams;

  for (const auto &entry : this->strandMap) {
    const auto &strandId = entry.first;
    if (strandId.dynStreamId == dynId) {
      auto dynS = entry.second;

      auto config = dynS->getConfig();
      if (config->streamConfig) {
        // Get the stream config from strand config.
        config = config->streamConfig;
      }

      S = dynS->getStaticStream();
      addrGenCb = config->addrGenCallback;
      formalParams = config->addrGenFormalParams;

      break;
    }
  }

  assert(S && "Not a Stream?");

  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(addrGenCb);
  if (!linearAddrGen) {
    // We cannot handle non-affine stream.
    return;
  }

  auto nucaManager =
      S->getCPUDelegator()->getSingleThreadContext()->getStreamNUCAManager();

  auto initVAddr =
      linearAddrGen
          ->genAddr(0,
                    convertFormalParamToParam(formalParams, getStreamValueFail))
          .uint64();

  Addr initPAddr;
  if (!S->getCPUDelegator()->translateVAddrOracle(initVAddr, initPAddr)) {
    return;
  }

  const auto &region = nucaManager->getContainingStreamRegion(initVAddr);
  auto nucaMapEntry = StreamNUCAMap::getRangeMapContaining(initPAddr);

  /**
   * As some heuristic, check that initVAddr is the same as regionStartVAddr.
   * TODO: Really check that the stream accessed the whole region.
   */
  if (!nucaMapEntry || !nucaMapEntry->isStreamPUM) {
    // So far only enable this feature for PUM region.
    return;
  }
  if (region.vaddr != initVAddr) {
    return;
  }
  MLC_S_DPRINTF(dynId, "Mark Cached PUMRegion %s.\n", region.name);
  nucaManager->markRegionCached(region.vaddr);
}

void MLCStrandManager::endStream(const DynStreamId &endId, MasterID masterId) {
  MLC_S_DPRINTF_(MLCRubyStreamLife, endId, "Received StreamEnd.\n");

  this->tryMarkPUMRegionCached(endId);

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
   * 1. If strendId < targetStrandId:
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

  auto targetStrandId =
      firstConfig->getStrandIdFromStreamElemIdx(streamElemIdx);

  auto interleaveCount =
      streamElemIdx / (splitInfo.getInterleave() * splitInfo.getTotalStrands());
  for (auto strandIdx = 0; strandIdx < splitInfo.getTotalStrands();
       ++strandIdx) {
    auto strandId =
        DynStrandId(streamId, strandIdx, splitInfo.getTotalStrands());
    auto dynS = this->mlcSE->getStreamFromStrandId(strandId);
    assert(dynS && "MLCDynS already released.");

    uint64_t checkStrandElemIdx = 0;
    if (strandIdx < targetStrandId.strandIdx) {
      checkStrandElemIdx =
          (interleaveCount + 1) * splitInfo.getInterleave() - 1;
    } else if (strandIdx > targetStrandId.strandIdx) {
      checkStrandElemIdx = interleaveCount * splitInfo.getInterleave();
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
