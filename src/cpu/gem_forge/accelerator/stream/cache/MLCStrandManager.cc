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

namespace gem5 {

MLCStrandManager::MLCStrandManager(MLCStreamEngine *_mlcSE)
    : mlcSE(_mlcSE), controller(_mlcSE->controller) {
  {
    std::ostringstream s;
    ccprintf(s, "[MLC_Reuse%d]", this->controller->getMachineID().num);
    this->reuseAnalyzer = std::make_unique<StreamReuseAnalyzer>(s.str());
  }
}

MLCStrandManager::~MLCStrandManager() {
  for (auto &idStream : this->strandMap) {
    delete idStream.second;
    idStream.second = nullptr;
  }
  this->strandMap.clear();
}

void MLCStrandManager::receiveStreamConfigure(ConfigVec *configs,
                                              RequestorID requestorId) {

  // auto configs = *(pkt->getPtr<ConfigVec *>());

  this->checkShouldBeSliced(*configs);

  StrandSplitContext splitContext;
  // So far we always split into 64 strands.
  splitContext.totalStrands =
      StreamNUCAMap::getNumRows() * StreamNUCAMap::getNumCols();
  for (const auto &config : *configs) {
    if (config->stream->getStreamName().find("gap.pr_push.atomic.out_v.ld") !=
        std::string::npos) {
      splitContext.totalStrands = 8;
    }
    if (config->stream->getStreamName().find("gap.bfs_push.out_v.ld") !=
        std::string::npos) {
      if (config->getTotalTripCount() <= 32) {
        splitContext.totalStrands = 2;
      } else if (config->getTotalTripCount() <= 48) {
        splitContext.totalStrands = 3;
      } else if (config->getTotalTripCount() <= 64) {
        splitContext.totalStrands = 4;
      } else if (config->getTotalTripCount() <= 80) {
        splitContext.totalStrands = 5;
      } else if (config->getTotalTripCount() <= 96) {
        splitContext.totalStrands = 6;
      } else if (config->getTotalTripCount() <= 112) {
        splitContext.totalStrands = 7;
      } else if (config->getTotalTripCount() <= 256) {
        splitContext.totalStrands = 8;
      } else {
        splitContext.totalStrands = 16;
      }
    }
  }
  if (this->canSplitIntoStrands(splitContext, *configs)) {
    this->splitIntoStrands(splitContext, *configs);
  }

  // Set the disableMigration flag for all configs.
  if (this->controller->myParams->stream_pum_fix_stream_at_req_bank) {
    for (auto config : *configs) {
      config->disableMigration = true;
    }
  }

  mlcSE->computeReuseInformation(*configs);
  for (auto config : *configs) {
    this->configureStream(config, requestorId);
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
                      "Disabled Slicing with StoreFunc with InnerValBaseS.\n");
        config->shouldBeSlicedToCacheLines = false;
      } else if (!config->stream->isLoopEliminated()) {
        // The core still needs to step one by one.
        MLC_S_DPRINTF(config->dynamicId,
                      "Disabled Slicing with StoreFunc as not Eliminated.\n");
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
   * Some hack for graph workloads.
   * 1. Avoid split outer loop streams.
   * 2. Only split the inner loop streams if they are long enough.
   * 3. If SplitByElem is enabled, we pass to canSplitIntoStrandsByElem.
   */
  for (const auto &config : configs) {
    if (config->stream->getStreamName().find("gf_warm_impl") !=
        std::string::npos) {
      return false;
    }
    if (config->stream->getStreamName().find(
            "gap.pr_push.atomic.out_begin.ld") != std::string::npos) {
      return false;
    }
    if (config->stream->getStreamName().find("gap.pr_push.update.score.ld") !=
        std::string::npos) {
      return false;
    }
    if (config->stream->getStreamName().find("gap.pr_push.atomic.out_v.ld") !=
        std::string::npos) {
      if (this->mlcSE->controller->myParams->enable_stream_strand_elem_split) {
        return this->canSplitIntoStrandsByElem(context, configs);
      }
      if (config->hasTotalTripCount() && config->getTotalTripCount() < 128) {
        return false;
      }
    }
    if (config->stream->getStreamName().find("gap.bfs_push.u.ld") !=
        std::string::npos) {
      return false;
    }
    if (config->stream->getStreamName().find("gap.bfs_push.out_v.ld") !=
        std::string::npos) {
      if (this->mlcSE->controller->myParams->enable_stream_strand_elem_split) {
        return this->canSplitIntoStrandsByElem(context, configs);
      }
      if (config->hasTotalTripCount() && config->getTotalTripCount() < 16) {
        return false;
      }
    }
    if (config->stream->getStreamName().find("gap.sssp.frontier.ld") !=
        std::string::npos) {
      return false;
    }
    if (config->stream->getStreamName().find("gap.sssp.out_w.ld") !=
        std::string::npos) {
      if (this->mlcSE->controller->myParams->enable_stream_strand_elem_split) {
        return this->canSplitIntoStrandsByElem(context, configs);
      }
      if (config->hasTotalTripCount() && config->getTotalTripCount() < 16) {
        return false;
      }
    }
  }

  if (!this->chooseNoSplitOuterTrip(context, configs)) {
    return false;
  }

  for (const auto &config : configs) {
    if (!this->precheckSplitable(context, config)) {
      return false;
    }
  }

  if (!this->chooseSplitDimIntrlv(context, configs)) {
    return false;
  }

  if (context.skipSanityCheck) {
    return true;
  }

  // Some additional sanity check.
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

bool MLCStrandManager::canSplitIntoStrandsByElem(
    StrandSplitContext &context, const ConfigVec &configs) const {

  // Should have only one direct config.
  assert(configs.size() == 1);
  auto config = configs.front();

  const auto &params = config->addrGenFormalParams;
  auto callback = config->addrGenCallback;

  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(callback);
  assert(linearAddrGen && "Callback is not linear.");
  assert(params.size() == 3);

  assert(config->hasTotalTripCount());

  auto totalElems = config->getTotalTripCount();
  assert(totalElems > 0 && "Empty stream should not be floated.");

  auto cpuDelegator = config->stream->getCPUDelegator();

  auto getBank = [cpuDelegator, linearAddrGen,
                  &params](uint64_t elemIdx) -> int {
    auto vaddr =
        linearAddrGen
            ->genAddr(elemIdx,
                      convertFormalParamToParam(params, getStreamValueFail))
            .uint64();
    auto vaddrLine = ruby::makeLineAddress(vaddr);
    Addr paddrLine;
    panic_if(!cpuDelegator->translateVAddrOracle(vaddrLine, paddrLine),
             "Failed to translate.");
    auto bank = StreamNUCAMap::getBank(paddrLine);
    assert(bank != -1);
    return bank;
  };

  // Get the first bank.
  auto prevBank = getBank(0);

  std::vector<uint64_t> elemSplits;
  for (uint64_t elemIdx = 0; elemIdx < totalElems; ++elemIdx) {
    auto bank = getBank(elemIdx);

    if (bank != prevBank) {
      // Split into strands.
      elemSplits.push_back(elemIdx);
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[ElemSplit] Elem %lu Bank %d -> %d.\n", elemIdx, prevBank,
                  bank);
    }

    prevBank = bank;
  }

  // Always push totalElems as last one.
  STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
              "[ElemSplit] EndElem %lu Bank %d.\n", totalElems, prevBank);
  elemSplits.push_back(totalElems);

  context.splitByElem = true;
  context.splitByElemInfo = StrandSplitInfo(elemSplits, elemSplits.size());

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
  auto &psc = context.perStreamContext
                  .emplace(std::piecewise_construct,
                           std::forward_as_tuple(config->dynamicId),
                           std::forward_as_tuple())
                  .first->second;
  const auto memChannelIntrlv = 4096;
  const auto llcBankIntrlv = 1024;
  if (config->floatPlan.isFloatedToMem()) {
    // We assume MemCtrl interleavs at 4kB -> 64 cache lines.
    psc.splitTripPerStrand =
        memChannelIntrlv / ruby::RubySystem::getBlockSizeBytes();
  } else {
    // We assume LLC interleavs at 1kB -> 16 cache lines.
    psc.splitTripPerStrand =
        llcBankIntrlv / ruby::RubySystem::getBlockSizeBytes();
  }

  // 1.
  if (!config->hasTotalTripCount()) {
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "[NoSplit] No TripCount.\n");
    return false;
  }
  if (config->getTotalTripCount() < 128) {
    /**
     * HACK: For ASPLOS I force split for array_sum_split2d.
     */
    if (config->stream->getStreamName().find("omp_array_sum_avx") !=
        std::string::npos) {
    } else {
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[NoSplit] Short TripCount %ld.\n",
                  config->getTotalTripCount());
      return false;
    }
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

bool MLCStrandManager::chooseNoSplitOuterTrip(StrandSplitContext &context,
                                              const ConfigVec &configs) const {

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

  if (this->controller->myParams->stream_strand_broadcast_size <= 1) {
    return true;
  }

  for (auto &config : configs) {

    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "[Strand] Analyzing Pattern %s.\n",
                printAffinePatternParams(config->addrGenFormalParams));

    std::vector<int64_t> trips;
    std::vector<int64_t> strides;
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

    /**
     * If we enable the strand broadcast, we try to split at some reused
     * dimention to enable broadcast.
     */
    int64_t outerTripCount = 1;
    bool foundReusedDim = false;
    for (int dim = trips.size() - 1; dim >= 0; --dim) {
      if (strides.at(dim) == 0) {
        // This is a reused dim.
        foundReusedDim = true;
        break;
      }
      outerTripCount *= trips.at(dim);
    }
    if (foundReusedDim) {
      // For now just enable this for kmeans/pointnet.
      if (context.noSplitOuterTrip < outerTripCount) {
        if (config->stream->streamName.find("gfm.kmeans.B.ld") !=
                std::string::npos ||
            config->stream->streamName.find("pointnet") != std::string::npos) {
          context.noSplitOuterTrip = outerTripCount;
          STRAND_LOG_(
              MLCRubyStrandSplit, config->dynamicId,
              "[Strand] Override NoSplitOuterTrip to %ld Due to Reuse.\n",
              context.noSplitOuterTrip);
        }
      }
    }
  }

  return true;
}

bool MLCStrandManager::chooseSplitDimIntrlv(StrandSplitContext &context,
                                            const ConfigVec &configs) const {

  // Try mm_outer special case.
  if (this->chooseSplitDimIntrlvMMOuter(context, configs)) {
    return true;
  }

  // Check if the split dim is defined by user.
  if (this->chooseSplitDimIntrlvByUser(context, configs)) {
    return true;
  }

  for (const auto &config : configs) {
    if (!this->chooseSplitDimIntrlv(context, config)) {
      return false;
    }
  }

  return true;
}

bool MLCStrandManager::chooseSplitDimIntrlvMMOuter(
    StrandSplitContext &context, const ConfigVec &configs) const {

  /**
   * A special case handling MM outer. Split on both dimensions.
   */
  if (configs.size() != 3) {
    return false;
  }

  ConfigPtr configA = nullptr;
  ConfigPtr configB = nullptr;
  ConfigPtr configC = nullptr;
  for (auto &config : configs) {
    std::string streamName = config->dynamicId.streamName;
    if (streamName.find("gfm.mm_outer.A") != std::string::npos) {
      configA = config;
    }
    if (streamName.find("gfm.mm_outer.B") != std::string::npos) {
      configB = config;
    }
    if (streamName.find("gfm.mm_outer.C") != std::string::npos) {
      configC = config;
    }
  }
  if (!configA || !configB || !configC) {
    return false;
  }

  STRAND_LOG_(MLCRubyStrandSplit, configA->dynamicId,
              "[Strand] Handle as mm_outer: %s.\n",
              printAffinePatternParams(configA->addrGenFormalParams));
  STRAND_LOG_(MLCRubyStrandSplit, configB->dynamicId,
              "[Strand] Handle as mm_outer: %s.\n",
              printAffinePatternParams(configB->addrGenFormalParams));
  STRAND_LOG_(MLCRubyStrandSplit, configC->dynamicId,
              "[Strand] Handle as mm_outer: %s.\n",
              printAffinePatternParams(configC->addrGenFormalParams));

  auto &pscA = context.perStreamContext.at(configA->dynamicId);
  auto &pscB = context.perStreamContext.at(configB->dynamicId);
  auto &pscC = context.perStreamContext.at(configC->dynamicId);

  extractStrideAndTripFromAffinePatternParams(configA->addrGenFormalParams,
                                              pscA.strides, pscA.trips);
  extractStrideAndTripFromAffinePatternParams(configB->addrGenFormalParams,
                                              pscB.strides, pscB.trips);
  extractStrideAndTripFromAffinePatternParams(configC->addrGenFormalParams,
                                              pscC.strides, pscC.trips);

  // Let's first split at inner dimension, and keep A not splited.
  auto splitDim = 0;
  auto splitDimTrip = pscB.trips.at(splitDim);
  auto outerTrip = AffinePattern::reduce_mul(pscB.trips.begin() + splitDim + 1,
                                             pscB.trips.end(), 1);
  auto totalTrip =
      AffinePattern::reduce_mul(pscB.trips.begin(), pscB.trips.end(), 1);
  auto innerTrip = totalTrip / outerTrip / splitDimTrip;
  auto splitCount = 8;

  auto splitDimTripPerStrand = (splitDimTrip + splitCount - 1) / splitCount;

  STRAND_LOG_(MLCRubyStrandSplit, configB->dynamicId,
              "[Strand] SplitDim %d SplitDimTrip %lu SplitCount %d "
              "SplitDimTrip/Strand %lu.\n",
              splitDim, splitDimTrip, splitCount, splitDimTripPerStrand);

  context.totalStrands = splitCount;
  context.skipSanityCheck = true;
  // So far do not split A.
  pscB.splitDims.emplace_back(splitDim, splitCount, splitDimTripPerStrand);
  pscC.splitDims.emplace_back(splitDim, splitCount, splitDimTripPerStrand);
  pscB.innerTrip = innerTrip;
  pscC.innerTrip = innerTrip;
  pscB.outerTrip = outerTrip;
  pscC.outerTrip = outerTrip;
  pscB.splitTripPerStrand = splitDimTripPerStrand;
  pscC.splitTripPerStrand = splitDimTripPerStrand;

  // Lets also split B and C on the second dimension, with 1 interleave.
  pscB.splitDims.emplace_back(1, 8, 1);
  pscC.splitDims.emplace_back(1, 8, 1);

  // A is split on the first dimension (which is the outer loop, with 1
  // interleave).
  pscA.splitDims.emplace_back(0, 8, 1);

  return true;
}

bool MLCStrandManager::chooseSplitDimIntrlvByUser(
    StrandSplitContext &context, const ConfigVec &configs) const {

  ConfigPtr configWithUserDefinedStrandSplit = nullptr;
  for (auto &config : configs) {
    auto S = config->stream;
    if (!S->getUserDefinedStrandSplit().empty()) {
      configWithUserDefinedStrandSplit = config;
      break;
    }
  }

  if (!configWithUserDefinedStrandSplit) {
    return false;
  }

  /**
   * We assume that:
   * 1. All split dimensions has interleave 1.
   * 2. The total strands is reasonable (<= 64) so far.
   */
  const auto &userDefinedStrandSplitDims =
      configWithUserDefinedStrandSplit->stream->getUserDefinedStrandSplit();
  {
    std::stringstream ss;
    for (auto splitLoop : userDefinedStrandSplitDims) {
      ss << splitLoop << ' ';
    }
    STRAND_LOG_(MLCRubyStrandSplit, configWithUserDefinedStrandSplit->dynamicId,
                "[Strand] UserStrandSplit with Dims %s.\n", ss.str());
  }

  context.totalStrands = 64;
  context.skipSanityCheck = true;

  for (auto &config : configs) {
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "[Strand] Handling %s.\n",
                printAffinePatternParams(config->addrGenFormalParams));

    auto &psc = context.perStreamContext.at(config->dynamicId);
    extractStrideAndTripFromAffinePatternParams(config->addrGenFormalParams,
                                                psc.strides, psc.trips);

    auto totalStrands = 1;

    // Be careful that strides and trips are ordered from inner to outer.
    // We process the split loop also from inner to outer.
    for (auto splitLevelIter = userDefinedStrandSplitDims.rbegin(),
              splitLevelEnd = userDefinedStrandSplitDims.rend();
         splitLevelIter != splitLevelEnd; ++splitLevelIter) {

      const auto &splitLevel = *splitLevelIter;

      if (splitLevel >= psc.trips.size()) {
        MLC_S_PANIC_NO_DUMP(
            config->dynamicId, "Overflown SplitLevel %d >= %s.", splitLevel,
            printAffinePatternParams(config->addrGenFormalParams));
      }

      auto splitLevelTrip = *(psc.trips.rbegin() + splitLevel);
      totalStrands *= splitLevelTrip;

      const int splitInterleave = 1;
      // SplitDims in PSC is ordered from inner to outer.
      psc.splitDims.emplace_back(psc.trips.size() - splitLevel - 1,
                                 splitLevelTrip, splitInterleave);

      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[Strand] SplitAt %d Trip %ld TotalSplit %ld.\n", splitLevel,
                  splitLevelTrip, totalStrands);
    }
  }

  return true;
}

bool MLCStrandManager::chooseSplitDimIntrlv(StrandSplitContext &context,
                                            ConfigPtr config) const {

  /**
   * Basically pick the SplitDim and Interleave.
   */

  auto &psc = context.perStreamContext.at(config->dynamicId);

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
  auto &trips = psc.trips;
  auto &strides = psc.strides;
  extractStrideAndTripFromAffinePatternParams(config->addrGenFormalParams,
                                              strides, trips);

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
  auto &outerTrip = psc.outerTrip;
  auto &innerTrip = psc.innerTrip;
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
  if (region && region->interleaves.size() == 1 &&
      region->interleaves.front() != 0) {
    if (!region->isStreamPUM) {
      const auto llcBankIntrlv = region->interleaves.front();
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

  psc.splitDims.emplace_back(splitDim, splitCount, splitDimTripPerStrand);
  psc.splitTripPerStrand = splitDimTripPerStrand;

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
  assert(!firstStreamContext.splitDims.empty() && "Cannot handle this now.");
  auto firstSplitDim = firstStreamContext.splitDims.front().dim;
  auto firstSplitDimTrip = firstStreamContext.trips.at(firstSplitDim);

  auto minSplitDimIntrlv = firstSplitDimTrip;

  for (const auto &config : configs) {
    const auto &psc = context.perStreamContext.at(config->dynamicId);
    auto dims = psc.trips.size();
    assert(psc.splitDims.size() == 1 && "Can only handle single dim split.");
    auto splitDim = psc.splitDims.front().dim;
    auto splitDimTrip = psc.trips.at(splitDim);
    if (dims - splitDim != firstDims - firstSplitDim) {
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[Strand] Mismatch in SplitDim %d-%d %d-%d %s.\n", dims,
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

    auto splitDimIntrlv = psc.splitDims.front().intrlv;
    if (splitDimIntrlv < minSplitDimIntrlv) {
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[Strand] Min SplitDimIntrlv %d.\n", splitDimIntrlv);
      minSplitDimIntrlv = splitDimIntrlv;
    }
  }

  for (const auto &config : configs) {
    auto &psc = context.perStreamContext.at(config->dynamicId);
    assert(psc.splitDims.size() == 1);
    auto splitDimIntrlv = psc.splitDims.front().intrlv;
    if (splitDimIntrlv > minSplitDimIntrlv) {
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[Strand] Adjust SplitDimIntrlv %d -> %d.\n", splitDimIntrlv,
                  minSplitDimIntrlv);
      psc.splitDims.front().intrlv = minSplitDimIntrlv;
    }

    // /**
    //  * Check if we need to handle TailInterleave.
    //  */
    // auto splitDim = psc.splitDim;
    // auto splitDimTrip = psc.trips.at(splitDim);
    // auto splitCount = context.totalStrands;
    // auto splitDimTripPerStrand = (splitDimTrip + splitCount - 1) /
    // splitCount; auto innerTrip = psc.innerTrip; if
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
    //     psc.splitTailInterleave = innerTrip * (v -
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

  auto &psc = context.perStreamContext.at(config->dynamicId);
  auto totalTrip = config->getTotalTripCount();
  const auto &outerTrip = psc.outerTrip;
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
    auto totalInterleave =
        psc.splitTripPerStrand * psc.innerTrip * context.totalStrands;
    if (config->pumElemPerSync < totalInterleave) {
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[Strand] NoSplit PUMElemPerSync %ld < Intrlv %ld * %ld * "
                  "Strands %d.\n",
                  config->pumElemPerSync, psc.splitTripPerStrand, psc.innerTrip,
                  context.totalStrands);
      return false;
    }
    if ((config->pumElemPerSync % totalInterleave) != 0) {
      STRAND_LOG_(
          MLCRubyStrandSplit, config->dynamicId,
          "[Strand] NoSplit PUMElemPerSync %ld %% (%d * %ld * %d) != 0.\n",
          config->pumElemPerSync, psc.splitTripPerStrand, psc.innerTrip,
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

  // Split into strands.
  StreamToStrandsMap streamToStrandMap;
  for (auto &config : streamConfigs) {
    auto &strands =
        streamToStrandMap
            .emplace(std::piecewise_construct, std::forward_as_tuple(config),
                     std::forward_as_tuple())
            .first->second;
    strands = this->splitIntoStrands(context, config);
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "---------------Split Strands\n");
    for (const auto &strand : strands) {
      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId, "Strand %3d %s.\n",
                  strand->getStrandId().strandIdx,
                  printAffinePatternParams(strand->addrGenFormalParams));
    }
  }

  // Some post process after the initial split and insert into configs.
  for (auto &split : streamToStrandMap) {
    auto &strands = split.second;
    this->mergeBroadcastStrands(context, streamToStrandMap, strands);
    configs.insert(configs.end(), strands.begin(), strands.end());
  }

  // // Split and insert into configs.
  // for (auto &config : streamConfigs) {
  //   auto strandConfigs = this->splitIntoStrands(context, config);
  //   configs.insert(configs.end(), strandConfigs.begin(),
  //   strandConfigs.end()); STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
  //               "---------------Split Strands\n");
  //   for (const auto &strandConfig : strandConfigs) {
  //     STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId, "Strand %s %s.\n",
  //                 DynStrandId(strandConfig->dynamicId,
  //                 strandConfig->strandIdx,
  //                             strandConfig->totalStrands),
  //                 printAffinePatternParams(strandConfig->addrGenFormalParams));
  //   }
  // }

  // Fix the reused SendTo relationship.
  this->fixReusedSendTo(context, streamConfigs, configs);

  // Recognize reused tile.
  for (auto strand : configs) {
    this->reuseLoadTile(context, strand);
    this->reuseStoreTile(context, strand);
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

  if (context.splitByElem) {
    // New implementation to split by element.
    bool isDirect = true;
    return this->splitIntoStrandsImpl(context, config, context.splitByElemInfo,
                                      isDirect);
  }

  auto &psc = context.perStreamContext.at(config->dynamicId);

  if (psc.splitDims.empty()) {
    // Don't split this Stream.
    ConfigVec strands;
    strands.push_back(config);
    return strands;
  }

  bool isDirect = true;
  StrandSplitInfo strandSplit(psc.trips, psc.splitDims);
  return this->splitIntoStrandsImpl(context, config, strandSplit, isDirect);
}

MLCStrandManager::ConfigVec MLCStrandManager::splitIntoStrandsImpl(
    StrandSplitContext &context, ConfigPtr config, StrandSplitInfo strandSplit,
    bool isDirect) {

  if (isDirect) {
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "[StrandSplit] ---------- Start Split Direct. Original "
                "AddrPat %s.\n",
                printAffinePatternParams(config->addrGenFormalParams));
  } else {
    STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                "[StrandSplit] ---------- Start Split Indirect.\n");
    /*********************************************************************
     * Now that IndS may have reuse on the BaseS. Adjust the StrandSplit.
     *********************************************************************/
    for (const auto &base : config->baseEdges) {
      if (base.isUsedBy) {
        if (base.reuseInfo.hasReuse()) {
          panic("Split on IndReuse is not supported yet: %s.",
                config->dynamicId);
          // strandSplit.setInnerTrip(strandSplit.getInnerTrip() * base.reuse);
          // STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
          //             "[StrandSplit] Adjust Intrlv by IndReuse %d -> %ld.\n",
          //             base.reuse, strandSplit.getInterleave());
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

      STRAND_LOG_(MLCRubyStrandSplit, config->dynamicId,
                  "[StrandSplit] StrandIdx %d AddrPat %s.\n", strandIdx,
                  printAffinePatternParams(strandAddrGenFormalParams));

      strand->addrGenFormalParams = strandAddrGenFormalParams;
      strand->totalTripCount = strandSplit.getStrandTripCount(
          config->getTotalTripCount(), strandIdx);
      strand->initVAddr = ruby::makeLineAddress(
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
        strand->addSendTo(dep.data, dep.reuseInfo, dep.skip);
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
        strand->addBaseAffineIV(baseConfig, base.reuseInfo, base.skip);
      } else if (base.isPredBy) {
        strand->addPredBy(baseConfig, base.reuseInfo, base.skip, base.predId,
                          base.predValue);
      } else {
        strand->addBaseOn(baseConfig, base.reuseInfo, base.skip);
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
    // Properly handle the PredBy info.
    bool isPredBy = false;
    int predId = 0;
    bool predValue = false;
    {
      bool foundUsedByEdge = false;
      for (const auto &baseEdge : dep.data->baseEdges) {
        if (baseEdge.isUsedBy) {
          foundUsedByEdge = true;
          assert(baseEdge.dynStreamId == config->dynamicId &&
                 "Mismatch UsedBy Edge.");
          isPredBy = baseEdge.isPredBy;
          predId = baseEdge.predId;
          predValue = baseEdge.predValue;
          break;
        }
      }
      panic_if(!foundUsedByEdge, "Miss BaseUsedByEdge.");
    }
    bool isDirect = false;
    auto depStrands =
        this->splitIntoStrandsImpl(context, dep.data, strandSplit, isDirect);
    assert(depStrands.size() == strands.size());
    for (int strandIdx = 0; strandIdx < depStrands.size(); ++strandIdx) {
      auto strand = strands.at(strandIdx);
      auto depStrand = depStrands.at(strandIdx);
      strand->addUsedBy(depStrand, dep.reuseInfo, isPredBy, predId, predValue);
      depStrand->totalTripCount =
          strand->getTotalTripCount() * dep.reuseInfo.getTotalReuse();
    }
  }

  return strands;
}

void MLCStrandManager::mergeBroadcastStrands(
    StrandSplitContext &context, StreamToStrandsMap &streamToStrandMap,
    CacheStreamConfigureVec &strands) {

  /**
   * We can merge iff:
   * 1. All strands have the same address pattern.
   * 2. No indirect streams, only SendToEdges.
   * 3. LoadStream but no computation.
   */
  if (this->controller->myParams->stream_strand_broadcast_size <= 1) {
    return;
  }
  if (strands.size() <= 1) {
    return;
  }

  auto firstStrand = strands.front();
  if (!firstStrand->baseEdges.empty()) {
    STRAND_LOG_(MLCRubyStrandSplit, firstStrand->dynamicId,
                "[NoBroadcast] Has BaseEdge.\n");
    return;
  }
  for (const auto &dep : firstStrand->depEdges) {
    if (dep.type != CacheStreamConfigureData::DepEdge::Type::SendTo) {
      STRAND_LOG_(MLCRubyStrandSplit, firstStrand->dynamicId,
                  "[NoBroadcast] Has NonSendTo DepEdge.\n");
      return;
    }
    if (dep.skip != 0) {
      STRAND_LOG_(MLCRubyStrandSplit, firstStrand->dynamicId,
                  "[NoBroadcast] Has SendTo Skip %d.\n", dep.skip);
      return;
    }
    if (dep.reuseInfo.hasReuse()) {
      // We support broadcast with reuse if the reuse is smaller than
      // SplitInnerTrip, which means the reuse is not splited.
      const auto &depDynId = dep.data->dynamicId;
      auto depInnerTrip = this->computeSplitInnerTrip(context, dep.data);

      if (dep.reuseInfo.getTotalReuse() <= depInnerTrip) {
        STRAND_LOG_(MLCRubyStrandSplit, firstStrand->dynamicId,
                    "[Broadcast] Reuse %s <= DepInnerTrip %ld of %s.\n",
                    dep.reuseInfo, depInnerTrip, depDynId);

      } else {
        STRAND_LOG_(MLCRubyStrandSplit, firstStrand->dynamicId,
                    "[NoBroadcast] Reuse %s > DepInnerTrip %ld of %s.\n",
                    dep.reuseInfo, depInnerTrip, depDynId);
        return;
      }
    }
  }

  auto S = firstStrand->stream;
  if (!S->isDirectLoadStream()) {
    STRAND_LOG_(MLCRubyStrandSplit, firstStrand->dynamicId,
                "[NoBroadcast] Not DirectLoadS.\n");
    return;
  }
  if (S->isLoadComputeStream()) {
    STRAND_LOG_(MLCRubyStrandSplit, firstStrand->dynamicId,
                "[NoBroadcast] LoadComputeS.\n");
    return;
  }

  /**
   * Group all strands by their patterns.
   * It's possible that some last strands have different parameters, and we
   * ignore them by not merging.
   */
  std::vector<CacheStreamConfigureVec> broadcastStrands;
  broadcastStrands.emplace_back();
  broadcastStrands.front().push_back(firstStrand);
  STRAND_LOG_(MLCRubyStrandSplit, firstStrand->getStrandId(),
              "[Broadcast] New BroadCastGroup.\n");
  for (auto mergedStrandIdxEnd = 1; mergedStrandIdxEnd < strands.size();
       ++mergedStrandIdxEnd) {
    auto strand = strands.at(mergedStrandIdxEnd);
    bool merged = false;
    for (auto &group : broadcastStrands) {
      if (isSameInvariantFormalParams(strand->addrGenFormalParams,
                                      group.front()->addrGenFormalParams)) {
        STRAND_LOG_(MLCRubyStrandSplit, strand->getStrandId(),
                    "[Broadcast] Params match with Strand %d.\n",
                    group.front()->strandIdx);
        group.push_back(strand);
        merged = true;
        break;
      }
    }
    if (!merged) {
      STRAND_LOG_(MLCRubyStrandSplit, strand->getStrandId(),
                  "[Broadcast] New BroadCastGroup.\n");
      broadcastStrands.emplace_back();
      broadcastStrands.back().push_back(strand);
    }
  }

  // Merge all groups.
  int mergeSize = this->controller->myParams->stream_strand_broadcast_size;
  CacheStreamConfigureVec mergedStrands;
  for (const auto &group : broadcastStrands) {

    for (auto i = 0; i < group.size(); i += mergeSize) {

      auto strandI = group.at(i);

      // Fix the SendTo.
      std::vector<CacheStreamConfigureData::DepEdge> fixedDepEdges;

      for (auto &dep : strandI->depEdges) {
        assert(dep.type == CacheStreamConfigureData::DepEdge::Type::SendTo);

        auto &recvStream = dep.data;
        auto &recvPSC = context.perStreamContext.at(recvStream->dynamicId);

        StrandSplitInfo recvStrandSplitInfo(recvPSC.trips, recvPSC.splitDims);
        auto recvS = recvStream->stream;

        int loopDiff = recvS->getLoopLevel() - S->getLoopLevel();
        if (loopDiff < 0) {
          // So far we don't broadcast for LoopDiff < 0, which should be Skip.
          fixedDepEdges.push_back(dep);
          continue;
        }

        if (!strandI->strandSplit.isHomogeniousSplitTo(recvStrandSplitInfo,
                                                       loopDiff)) {
          fixedDepEdges.push_back(dep);
          continue;
        }

        auto &recvStrands = streamToStrandMap.at(recvStream);

        for (auto &recvStrand : recvStrands) {

          for (auto j = i; j < i + mergeSize && j < group.size(); ++j) {

            auto strandJ = group.at(j);

            if (!strandJ->strandSplit.isHomogeniousMatch(
                    recvStrandSplitInfo, loopDiff, strandJ->strandIdx,
                    recvStrand->strandIdx)) {
              // This is not our recv strand.
              continue;
            }

            auto fixedDep = dep;
            fixedDep.data = recvStrand;

            STRAND_LOG_(MLCRubyStrandSplit, strandI->getStrandId(),
                        "[MergeBroadcast] Merged %3d -> %s R/S %s/%d!\n",
                        strandJ->strandIdx, recvStrand->getStrandId(),
                        fixedDep.reuseInfo, fixedDep.skip);
            fixedDepEdges.push_back(fixedDep);

            // Also fix the recv strand's reuse.
            __attribute__((unused)) bool fixedRecvReuse = false;
            for (auto &base : recvStrand->baseEdges) {
              if (base.dynStreamId == strandI->dynamicId) {
                assert(base.reuseInfo == dep.reuseInfo);
                base.data = strandI;
                base.reuseInfo = dep.reuseInfo;
                base.isStrandSendTo = true;
                fixedRecvReuse = true;
                break;
              }
            }
            assert(fixedRecvReuse);
            break;
          }
        }
      }

      // for (auto j = i + 1; j < i + mergeSize && j < group.size(); ++j) {

      //   auto strandJ = group.at(j);

      //   STRAND_LOG_(MLCRubyStrandSplit, strandJ->dynamicId,
      //               "[MergeBroadcast] Merged %d -> %d!\n",
      //               strandJ->strandIdx, strandI->strandIdx);
      //   strandI->broadcastStrands.push_back(strandJ);
      // }

      strandI->depEdges = fixedDepEdges;
      mergedStrands.push_back(strandI);
    }
  }

  // Just keep the lead strand.
  strands = mergedStrands;
}

DynStreamFormalParamV MLCStrandManager::splitAffinePattern(
    StrandSplitContext &context, ConfigPtr config,
    const StrandSplitInfo &strandSplit, int strandIdx) {

  if (strandSplit.isSplitByDim()) {

    return this->splitAffinePatternByDim(context, config, strandSplit,
                                         strandIdx);

  } else if (strandSplit.isSplitByElem()) {
    assert(config->hasTotalTripCount());
    auto startElem =
        strandSplit.mapStrandToStream(StrandElemSplitIdx(strandIdx, 0));
    auto strandTrip =
        strandSplit.getStrandTripCount(config->getTotalTripCount(), strandIdx);
    auto endElem = strandSplit.mapStrandToStream(
        StrandElemSplitIdx(strandIdx, strandTrip));
    return config->splitAffinePatternByElem(startElem, endElem, strandIdx,
                                            strandSplit.getTotalStrands());
  } else {
    panic("Unsupported StrandSplitInfo.");
  }
}

DynStreamFormalParamV MLCStrandManager::splitAffinePatternByDim(
    StrandSplitContext &context, ConfigPtr config,
    const StrandSplitInfo &strandSplit, int strandIdx) {

  const auto &psc = context.perStreamContext.at(config->dynamicId);

  DynStreamFormalParamV params = config->addrGenFormalParams;

  // Split from outer dimensions.
  for (int i = psc.splitDims.size() - 1; i >= 0; i--) {
    const auto &splitDim = psc.splitDims.at(i);
    auto strandId = strandSplit.getStrandIdAtDim(strandIdx, splitDim.dim);
    params =
        this->splitAffinePatternAtDim(config->dynamicId, params, splitDim.dim,
                                      splitDim.intrlv, splitDim.cnt, strandId);
  }

  return params;
}

DynStreamFormalParamV MLCStrandManager::splitAffinePatternAtDim(
    const DynStreamId &dynId, const DynStreamFormalParamV &params, int splitDim,
    int64_t interleave, int totalStrands, int strandIdx) {

  /**
   * * Split an AffineStream at SplitDim. This is similar to OpenMP static
   * * scheduling.
   * *   start : S1 : T1 : ... : Ss : Ts : ... : Sn : Tn
   *
   * * Tt = interleave
   * * Tn = Tt * totalStrands
   *
   * * ->
   * *   start + strandIdx * Ss * Tt
   * * : S1      : T1 : ...
   * * : Ss      : Tt
   * * : Ss * Tn : Ts / Tn + (strandIdx < (Ts % Tn) ? 1 : 0) : ...
   * * : Sn      : Tn
   *
   * * Notice that we have to take care when Ts % Tn != 0 by adding one to
   * * strands with strandIdx < (Ts % Tn).
   */

  std::vector<uint64_t> trips;
  std::vector<int64_t> strides;
  uint64_t prevTrip = 1;
  assert((params.size() % 2) == 1);
  for (int i = 0; i + 1 < params.size(); i += 2) {
    const auto &s = params.at(i);
    assert(s.isInvariant);
    strides.push_back(s.invariant.int64());

    const auto &t = params.at(i + 1);
    assert(t.isInvariant);
    auto trip = t.invariant.uint64();
    trips.push_back(trip / prevTrip);
    prevTrip = trip;
  }
  assert(!trips.empty());
  assert(splitDim < trips.size());

  auto splitDimTrip = trips.at(splitDim);
  auto splitDimStride = strides.at(splitDim);

  auto innerTrip = 1;
  for (int i = 0; i < splitDim; ++i) {
    innerTrip *= trips.at(i);
  }
  auto intrlvTrip = interleave;
  auto totalIntrlvTrip = intrlvTrip * totalStrands;

  auto start = params.back().invariant.uint64();
  auto strandStart = start + strandIdx * splitDimStride * intrlvTrip;

  // Copy the original params.
  DynStreamFormalParamV strandParams = params;

#define setTrip(dim, t)                                                        \
  {                                                                            \
    strandParams.at((dim) * 2 + 1).isInvariant = true;                         \
    strandParams.at((dim) * 2 + 1).invariant.uint64() = t;                     \
  }
#define setStride(dim, t)                                                      \
  {                                                                            \
    strandParams.at((dim) * 2).isInvariant = true;                             \
    strandParams.at((dim) * 2).invariant.uint64() = t;                         \
  }
#define setStart(t)                                                            \
  {                                                                            \
    strandParams.back().isInvariant = true;                                    \
    strandParams.back().invariant.uint64() = t;                                \
  }

  // Insert another dimension after SplitDim.
  strandParams.insert(strandParams.begin() + 2 * splitDim + 1,
                      DynStreamFormalParam());
  strandParams.insert(strandParams.begin() + 2 * splitDim + 1,
                      DynStreamFormalParam());

  // Adjust the strand start.
  setStart(strandStart);

  int64_t splitOutTrip = 1;
  int64_t splitTrip = intrlvTrip;

  DYN_S_DPRINTF_(
      MLCRubyStrandSplit, dynId,
      "Intrlv %d IntrlvTrip %d SplitDimTrip %d TotalStrands %d Pat %s.\n",
      interleave, intrlvTrip, splitDimTrip, totalStrands,
      printAffinePatternParams(params));

  if (totalIntrlvTrip <= splitDimTrip) {
    // Compute the SplitOutTrip.
    auto remainderTrip = splitDimTrip % totalIntrlvTrip;
    if (remainderTrip % intrlvTrip != 0) {
      if (splitDim + 1 != trips.size()) {
        DYN_S_PANIC(dynId,
                    "Cannot handle remainderTrip %ld %% intrlvTrip %ld != 0.",
                    remainderTrip, intrlvTrip);
      }
    }
    auto remainderStrandIdx = (remainderTrip + intrlvTrip - 1) / intrlvTrip;
    auto splitOutTripRemainder = (strandIdx < remainderStrandIdx) ? 1 : 0;
    splitOutTrip = splitDimTrip / totalIntrlvTrip + splitOutTripRemainder;

  } else {
    /**
     * Strands beyond FinalStrandIdx would have no trip count.
     */
    auto finalStrandIdx = splitDimTrip / intrlvTrip;
    if (strandIdx == finalStrandIdx) {
      splitTrip = splitDimTrip - finalStrandIdx * intrlvTrip;
    } else if (strandIdx > finalStrandIdx) {
      splitTrip = 0;
    }

    // In this case, SplitOutDimTrip is always 1.
    splitOutTrip = 1;
  }

  // Adjust the SplitOutDim.
  setTrip(splitDim, splitTrip * innerTrip);
  setStride(splitDim + 1, splitDimStride * totalIntrlvTrip);
  assert(splitOutTrip > 0);
  setTrip(splitDim + 1, splitOutTrip * splitTrip * innerTrip);

  // We need to fix all upper dimension's trip count.
  for (int dim = splitDim + 2; dim < trips.size() + 1; ++dim) {
    auto fixedOuterTrip =
        strandParams.at(dim * 2 - 1).invariant.uint64() * trips.at(dim - 1);
    setTrip(dim, fixedOuterTrip);
  }

#undef setTrip
#undef setStride
#undef setStart

  return strandParams;
}

int64_t MLCStrandManager::computeSplitInnerTrip(StrandSplitContext &context,
                                                const ConfigPtr &config) const {
  // We support broadcast with reuse if the reuse is smaller than
  // SplitInnerTrip, which means the reuse is not splited.
  const auto &psc = context.perStreamContext.at(config->dynamicId);

  auto firstSplitDim = psc.trips.size();
  if (!psc.splitDims.empty()) {
    firstSplitDim = psc.splitDims.front().dim;
  }
  auto splitInnerTrip = AffinePattern::reduce_mul(
      psc.trips.begin(), psc.trips.begin() + firstSplitDim, 1);
  return splitInnerTrip;
}

void MLCStrandManager::fixReusedSendTo(StrandSplitContext &context,
                                       ConfigVec &streamConfigs,
                                       ConfigVec &strandConfigs) {

  for (auto &strand : strandConfigs) {

    auto S = strand->stream;

    std::vector<CacheStreamConfigureData::DepEdge> fixedDepEdges;

    for (auto &dep : strand->depEdges) {
      if (dep.type != CacheStreamConfigureData::DepEdge::Type::SendTo) {
        fixedDepEdges.push_back(dep);
        continue;
      }
      if (!dep.reuseInfo.hasReuse()) {
        fixedDepEdges.push_back(dep);
        continue;
      }
      auto depSplitInnerTrip = this->computeSplitInnerTrip(context, dep.data);
      if (dep.reuseInfo.getTotalReuse() <= depSplitInnerTrip) {
        // The reuse is not splitted.
        fixedDepEdges.push_back(dep);
        continue;
      }

      auto &recvStream = dep.data;
      auto &recvPSC = context.perStreamContext.at(recvStream->dynamicId);
      StrandSplitInfo recvStrandSplitInfo(recvPSC.trips, recvPSC.splitDims);
      auto recvS = recvStream->stream;

      // Sending to inner loop.
      assert(recvS->getLoopLevel() >= S->getLoopLevel());

      int loopDiff = recvS->getLoopLevel() - S->getLoopLevel();

      if (!strand->strandSplit.isHomogeniousSplitTo(recvStrandSplitInfo,
                                                    loopDiff)) {
        // This is not homogenious split. We can not handle.
        if (strand->totalStrands != 1 || recvStream->totalStrands != 1) {
          MLC_S_PANIC_NO_DUMP(strand->getStrandId(),
                              "Illegal split between reuse send to %s.",
                              recvStream->dynamicId);
        }
        // Reuse is not splitted.
        fixedDepEdges.push_back(dep);
        continue;
      }

      // We need to split the SendTo to each Strand.
      STRAND_LOG_(MLCRubyStrandSplit, strand->getStrandId(),
                  "[ReuseSendTo] R/S %s/%ld -> %s.\n", dep.reuseInfo, dep.skip,
                  recvStream->getStrandId());

      for (auto &recvStrand : strandConfigs) {
        if (recvStrand->dynamicId != recvStream->dynamicId) {
          // This is not the recv strand.
          continue;
        }

        if (!strand->strandSplit.isHomogeniousMatch(recvStrandSplitInfo,
                                                    loopDiff, strand->strandIdx,
                                                    recvStrand->strandIdx)) {
          // This is not our recv strand.
          continue;
        }

        // Get the new reuse count by checking StrandTripCount.
        auto fixedReuse = recvStrandSplitInfo.getStrandTripCountByDim(
            recvStrand->strandIdx, loopDiff);

        // Dupliate the DepEdge and fix the reuse.
        auto fixedDep = dep;
        fixedDep.reuseInfo = StreamReuseInfo(fixedReuse);
        fixedDep.data = recvStrand;

        STRAND_LOG_(MLCRubyStrandSplit, strand->getStrandId(),
                    "[ReuseSendTo] Fixed R/S %s/%ld -> %s.\n",
                    fixedDep.reuseInfo, fixedDep.skip,
                    recvStrand->getStrandId());
        fixedDepEdges.push_back(fixedDep);

        // Also fix the recv strand's reuse.
        __attribute__((unused)) bool fixedRecvReuse = false;
        for (auto &base : recvStrand->baseEdges) {
          if (base.dynStreamId == strand->dynamicId) {
            assert(base.reuseInfo == dep.reuseInfo);
            base.data = strand;
            base.reuseInfo = fixedDep.reuseInfo;
            base.isStrandSendTo = true;
            fixedRecvReuse = true;
            break;
          }
        }
        assert(fixedRecvReuse);
      }
    }

    strand->depEdges = std::move(fixedDepEdges);
  }
}

void MLCStrandManager::reuseLoadTile(StrandSplitContext &context,
                                     ConfigPtr strand) {

  /**
   * We try to recognize reused tile for DirectLoadS that:
   * 1. Has no indirect stream.
   * 2. Reused tile size is smaller than the threshold.
   */
  if (this->controller->myParams->stream_reuse_tile_elems == 0) {
    return;
  }
  auto S = strand->stream;
  if (!S->isDirectLoadStream()) {
    // Only do this on DirectLoadStream.
    return;
  }

  if (!strand->baseEdges.empty()) {
    return;
  }

  for (auto &dep : strand->depEdges) {
    if (dep.type != CacheStreamConfigureData::DepEdge::Type::SendTo) {
      // This one has indirect stream. Can not handle reused tile.
      return;
    }
    if (dep.reuseInfo.hasReuse()) {
      if (!dep.reuseInfo.isInnerLoopReuse()) {
        return;
      }
    }
  }

  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(strand->addrGenCallback);
  if (!linearAddrGen) {
    return;
  }
  if (strand->addrGenFormalParams.size() % 2 != 1) {
    // Missing final trip.
    return;
  }

  auto reuseInfo = this->reuseAnalyzer->analyzeReuse(strand);
  if (!reuseInfo.hasReuse()) {
    return;
  }

  auto reuseCount = reuseInfo.getTotalReuse();

  std::vector<int64_t> strides;
  std::vector<int64_t> trips;
  extractStrideAndTripFromAffinePatternParams(strand->addrGenFormalParams,
                                              strides, trips);

  auto totalTrip = AffinePattern::reduce_mul(trips.begin(), trips.end(), 1);
  auto newTrip = totalTrip / reuseCount;

  // Change all send to edges.
  for (auto &dep : strand->depEdges) {
    auto finalReuseInfo = reuseInfo.mergeInnerLoopReuse(dep.reuseInfo);
    dep.reuseInfo = finalReuseInfo;
    auto &recvConfig = dep.data;
    bool __attribute__((unused)) foundBaseEdge = false;
    STRAND_LOG_(MLCRubyStrandSplit, strand->getStrandId(),
                "[ReuseTile] Fix %s -> %s.\n", finalReuseInfo,
                recvConfig->getStrandId());
    for (auto &base : recvConfig->baseEdges) {
      if (base.dynStreamId == strand->dynamicId) {
        assert(base.isStrandSendTo);
        base.reuseInfo = finalReuseInfo;
        foundBaseEdge = true;
        break;
      }
    }
    assert(foundBaseEdge);
  }

  // Erase the resued dim.
  reuseInfo.transformStrideAndTrip(strides, trips);
  auto startVAddr = strand->addrGenFormalParams.back().invariant.front();
  strand->addrGenFormalParams =
      constructFormalParamsFromStrideAndTrip(startVAddr, strides, trips);
  STRAND_LOG_(MLCRubyStrandSplit, strand->getStrandId(),
              "[ReuseTile] Trip %ld -> %ld New Affine Pat %s.\n", totalTrip,
              newTrip, printAffinePatternParams(strand->addrGenFormalParams));
  strand->totalTripCount = newTrip;
  strand->innerTripCount = newTrip;
}

void MLCStrandManager::reuseStoreTile(StrandSplitContext &context,
                                      ConfigPtr strand) {
  /**
   * We try to recognize reused tile for DirectStoreComputeS that:
   * 1. Has no indirect stream.
   * 2. Reused tile size is smaller than the threshold.
   */
  if (this->controller->myParams->stream_reuse_tile_elems == 0) {
    return;
  }
  auto S = strand->stream;
  if (!S->isDirectStoreStream() || !S->isStoreComputeStream()) {
    STRAND_LOG_(
        MLCRubyStrandSplit, strand->getStrandId(),
        "[ReuseTile] No StoreTile: Not DirectStore %d StoreCompute %d.\n",
        S->isDirectStoreStream(), S->isStoreComputeStream());
    return;
  }

  if (!strand->depEdges.empty()) {
    // What? StoreS with DepS?
    STRAND_LOG_(MLCRubyStrandSplit, strand->getStrandId(),
                "[ReuseTile] No StoreTile: Has DepEdge.\n");
    return;
  }

  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(strand->addrGenCallback);
  if (!linearAddrGen) {
    STRAND_LOG_(MLCRubyStrandSplit, strand->getStrandId(),
                "[ReuseTile] No StoreTile: Not LinearAddrGen.\n");
    return;
  }
  if (strand->addrGenFormalParams.size() % 2 != 1) {
    // Missing final trip.
    STRAND_LOG_(MLCRubyStrandSplit, strand->getStrandId(),
                "[ReuseTile] No StoreTile: No TotalTripCount.\n");
    return;
  }

  auto reuseInfo = this->reuseAnalyzer->analyzeReuse(strand);
  STRAND_LOG_(MLCRubyStrandSplit, strand->getStrandId(),
              "[ReuseTile] StoreTile %s.\n", reuseInfo);
  if (!reuseInfo.hasReuse()) {
    return;
  }

  strand->storeReuseInfo = reuseInfo;
}

void MLCStrandManager::configureStream(ConfigPtr config,
                                       RequestorID requestorId) {
  MLC_S_DPRINTF_(MLCRubyStreamLife,
                 DynStrandId(config->dynamicId, config->strandIdx),
                 "[Strand] Received StreamConfig, TotalTripCount %lu.\n",
                 config->totalTripCount);
  /**
   * Do not release the pkt and streamConfigureData as they should be
   * forwarded to the LLC bank and released there. However, we do need to fix
   * initPAddr to our LLC bank in case it is not valid. This has to
   * be done before initializing the MLCDynStream so that it knows the
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
   * ! We initialize the indirect stream first so that the direct stream's
   * ! constructor can start notify it about base stream data.
   * Use DFS to initialize all IndStreams.
   */
  std::vector<MLCDynIndirectStream *> indirectStreams;
  {
    std::vector<CacheStreamConfigureDataPtr> configStack;
    configStack.push_back(config);
    while (!configStack.empty()) {
      auto curConfig = configStack.back();
      configStack.pop_back();
      for (const auto &edge : curConfig->depEdges) {
        if (edge.type != CacheStreamConfigureData::DepEdge::UsedBy) {
          continue;
        }
        const auto &indConfig = edge.data;
        // Let's create an indirect stream.
        auto indS = new MLCDynIndirectStream(
            indConfig, this->controller, mlcSE->responseToUpperMsgBuffer,
            mlcSE->requestToLLCMsgBuffer,
            config->dynamicId /* Root dynamic stream id. */);
        this->strandMap.emplace(indS->getDynStrandId(), indS);
        indirectStreams.push_back(indS);
        configStack.push_back(indConfig);
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
  this->sendConfigToRemoteSE(config, requestorId);
}

void MLCStrandManager::sendConfigToRemoteSE(ConfigPtr config,
                                            RequestorID requestorId) {

  /**
   * Set the RemoteSE to LLC SE or Mem SE, depending on the FloatPlan on the
   * FirstFloatElemIdx.
   */
  auto firstFloatElemIdx = config->floatPlan.getFirstFloatElementIdx();
  auto firstFloatElemMachineType =
      config->floatPlan.getMachineTypeAtElem(firstFloatElemIdx);

  auto initPAddrLine = ruby::makeLineAddress(config->initPAddr);
  auto remoteSEMachineID = this->controller->mapAddressToLLCOrMem(
      initPAddrLine, firstFloatElemMachineType);

  if (config->disableMigration &&
      firstFloatElemMachineType != ruby::MachineType_L2Cache) {
    MLC_S_PANIC_NO_DUMP(config->dynamicId,
                        "Cannot disable migration for non-LLC stream.");
  }

  // Create a new packet.
  RequestPtr req = std::make_shared<Request>(config->initPAddr, sizeof(config),
                                             0, requestorId);
  PacketPtr pkt = new Packet(req, MemCmd::StreamConfigReq);
  uint8_t *pktData = reinterpret_cast<uint8_t *>(new ConfigPtr(config));
  pkt->dataDynamic(pktData);
  // Enqueue a configure packet to the target LLC bank.
  auto msg = std::make_shared<ruby::RequestMsg>(this->controller->clockEdge());
  msg->m_addr = initPAddrLine;
  msg->m_Type = ruby::CoherenceRequestType_STREAM_CONFIG;
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
    msg->m_MessageSize = ruby::MessageSizeType_Control;
  } else {
    msg->m_MessageSize = ruby::MessageSizeType_Data;
  }

  Cycles latency(1); // Just use 1 cycle latency here.

  MLC_S_DPRINTF(config->dynamicId, "Send Config to RemoteSE at %s.\n",
                remoteSEMachineID);

  mlcSE->requestToLLCMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(latency));
}

void MLCStrandManager::receiveStreamEnd(const std::vector<DynStreamId> &endIds,
                                        RequestorID requestorId) {
  for (const auto &endId : endIds) {
    this->endStream(endId, requestorId);
  }
}

void MLCStrandManager::tryMarkPUMRegionCached(const DynStreamId &dynId) {

  std::vector<std::pair<DynStrandId, std::pair<Addr, ruby::MachineType>>>
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

  auto region = nucaManager->tryGetContainingStreamRegion(initVAddr);
  auto nucaMapEntry = StreamNUCAMap::getRangeMapContaining(initPAddr);

  /**
   * As some heuristic, check that initVAddr is the same as regionStartVAddr.
   * TODO: Really check that the stream accessed the whole region.
   */
  if (!region || !nucaMapEntry || !nucaMapEntry->isStreamPUM) {
    // So far only enable this feature for PUM region.
    return;
  }
  if (region->vaddr != initVAddr) {
    return;
  }
  MLC_S_DPRINTF(dynId, "Mark Cached PUMRegion %s.\n", region->name);
  nucaManager->markRegionCached(region->vaddr);
}

void MLCStrandManager::endStream(const DynStreamId &endId,
                                 RequestorID requestorId) {
  MLC_S_DPRINTF_(MLCRubyStreamLife, endId, "Recv StreamEnd.\n");

  this->tryMarkPUMRegionCached(endId);

  /**
   * Find all root strands and record the PAddr and ruby::MachineType to
   * multicast the StreamEnd message.
   */
  std::vector<std::pair<DynStrandId, std::pair<Addr, ruby::MachineType>>>
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

    auto rootLLCStreamPAddrLine = ruby::makeLineAddress(rootLLCStreamPAddr);
    auto rootStreamOffloadedBank = this->controller->mapAddressToLLCOrMem(
        rootLLCStreamPAddrLine, rootStreamOffloadedMachineType);
    auto copyStrandId = new DynStrandId(strandId);
    RequestPtr req = std::make_shared<Request>(
        rootLLCStreamPAddrLine, sizeof(copyStrandId), 0, requestorId);
    PacketPtr pkt = new Packet(req, MemCmd::StreamEndReq);
    uint8_t *pktData = new uint8_t[req->getSize()];
    *(reinterpret_cast<uint64_t *>(pktData)) =
        reinterpret_cast<uint64_t>(copyStrandId);
    pkt->dataDynamic(pktData);

    if (this->controller->myParams->enable_stream_idea_end) {
      auto remoteController =
          ruby::AbstractStreamAwareController::getController(
              rootStreamOffloadedBank);
      auto remoteSE = remoteController->getLLCStreamEngine();
      // StreamAck is also disguised as StreamData.
      remoteSE->receiveStreamEnd(pkt);
      MLC_S_DPRINTF(strandId, "Send ideal StreamEnd to %s.\n",
                    rootStreamOffloadedBank);

    } else {
      // Enqueue a end packet to the target LLC bank.
      auto msg =
          std::make_shared<ruby::RequestMsg>(this->controller->clockEdge());
      msg->m_addr = rootLLCStreamPAddrLine;
      msg->m_Type = ruby::CoherenceRequestType_STREAM_END;
      msg->m_Requestors.add(this->controller->getMachineID());
      msg->m_Destination.add(rootStreamOffloadedBank);
      msg->m_MessageSize = ruby::MessageSizeType_Control;
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
  if (dynS && dynS->getDynStrandId().totalStrands != 1) {
    MLC_SLICE_PANIC_NO_DUMP(
        sliceId,
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
   * We need to check for all strands to see they have completed before the
   * target StreamElemIdx.
   */

  auto firstDynS = this->mlcSE->getStreamFromStrandId(DynStrandId(streamId));
  assert(firstDynS && "MLCDynS already released?");

  auto firstConfig = firstDynS->getConfig();
  const auto &splitInfo = firstConfig->strandSplit;

  auto targetStrandElemSplit = splitInfo.mapStreamToPrevStrand(streamElemIdx);

  for (auto strandIdx = 0; strandIdx < splitInfo.getTotalStrands();
       ++strandIdx) {
    auto strandId =
        DynStrandId(streamId, strandIdx, splitInfo.getTotalStrands());
    auto dynS = this->mlcSE->getStreamFromStrandId(strandId);
    assert(dynS && "MLCDynS already released.");

    uint64_t checkStrandElemIdx = targetStrandElemSplit.at(strandIdx).elemIdx;
    if (checkStrandElemIdx == -1) {
      // No need to check this one.
      continue;
    }
    if (!dynS->isElementAcked(checkStrandElemIdx)) {
      MLC_S_DPRINTF(dynS->getDynStrandId(), "NoAck for StrandElem %lu.\n",
                    checkStrandElemIdx);
      dynS->registerElementAckCallback(checkStrandElemIdx, callback);
      return false;
    }
  }

  return true;
}
} // namespace gem5
