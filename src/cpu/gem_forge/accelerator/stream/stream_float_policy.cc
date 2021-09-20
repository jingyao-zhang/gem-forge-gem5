#include "stream_float_policy.hh"

#include "mem/ruby/structures/CacheMemory.hh"
#include "stream_engine.hh"

#include "debug/StreamFloatPolicy.hh"
#define DEBUG_TYPE StreamFloatPolicy
#include "stream_log.hh"

OutputStream *StreamFloatPolicy::log = nullptr;

namespace {
std::vector<uint64_t> getCacheCapacity(StreamEngine *se) {
  /**
   * TODO: Handle classical memory system.
   */
  uint64_t l1Size = 0;
  uint64_t l2Size = 0;
  uint64_t l3Size = 0;
  for (auto so : se->getSimObjectList()) {
    auto cacheMemory = dynamic_cast<CacheMemory *>(so);
    if (!cacheMemory) {
      continue;
    }
    if (so->name() == "system.ruby.l0_cntrl0.Dcache") {
      // L1 data cache.
      l1Size = cacheMemory->getCacheSize();
    } else if (so->name() == "system.ruby.l1_cntrl0.cache") {
      // L2 cache.
      l2Size = cacheMemory->getCacheSize();
    } else if (so->name() == "system.ruby.l2_cntrl0.L2cache") {
      // } else if (so->name().find("system.ruby.l2_cntrl") == 0) {
      // This is the shared LLC.
      l3Size = cacheMemory->getCacheSize();
    }
  }
  assert(l1Size != 0 && "Failed to find L1 size.");
  assert(l2Size != 0 && "Failed to find L2 size.");
  if (l3Size <= l2Size) {
    panic("L1Size %lu L2Size %lu >= L3Size %lu.", l1Size, l2Size, l3Size);
  }
  std::vector<uint64_t> ret;
  ret.push_back(l1Size);
  ret.push_back(l2Size);
  ret.push_back(l3Size);
  DPRINTF(StreamFloatPolicy, "Get L1Size %d, L2Size %d.\n", l1Size, l2Size);
  return ret;
}

} // namespace

StreamFloatPolicy::StreamFloatPolicy(bool _enabled, bool _enabledFloatMem,
                                     const std::string &_policy,
                                     const std::string &_levelPolicy)
    : enabled(_enabled), enabledFloatMem(_enabledFloatMem) {
  if (_policy == "static") {
    this->policy = PolicyE::STATIC;
  } else if (_policy == "manual") {
    this->policy = PolicyE::MANUAL;
  } else if (_policy == "smart") {
    this->policy = PolicyE::SMART;
  } else if (_policy == "smart-computation") {
    this->policy = PolicyE::SMART_COMPUTATION;
  } else {
    panic("Invalid StreamFloatPolicy %s.", _policy);
  }

  if (_levelPolicy == "static") {
    this->levelPolicy = LevelPolicyE::LEVEL_STATIC;
  } else if (_levelPolicy == "smart") {
    this->levelPolicy = LevelPolicyE::LEVEL_SMART;
  } else {
    this->levelPolicy = LevelPolicyE::LEVEL_MANUAL;
  }

  // Initialize the output stream.
  if (log == nullptr) {
    auto directory = simout.findOrCreateSubdirectory("stream_float_policy");
    log = directory->create("log.log");
  }
}

StreamFloatPolicy::~StreamFloatPolicy() {
  if (log) {
    getLog() << std::flush;
    auto directory = simout.findOrCreateSubdirectory("stream_float_policy");
    directory->close(log);
    log = nullptr;
  }
}

std::ostream &StreamFloatPolicy::logStream(Stream *S) {
  return getLog() << S->getCPUId() << '-' << S->getStreamName() << ": ";
}

StreamFloatPolicy::FloatDecision
StreamFloatPolicy::shouldFloatStream(DynamicStream &dynS) {
  if (!this->enabled) {
    return FloatDecision();
  }
  // Initialize the private cache capacity.
  auto S = dynS.stream;
  if (this->cacheCapacity.empty()) {
    this->cacheCapacity = getCacheCapacity(S->se);
  }
  /**
   * This is the root of floating streams:
   * 1. DirectLoadStream.
   * 2. PointerChaseLoadStream.
   * 3. Direct Atomic/StoreStream without being merged, and StoreFunc enabled.
   */
  {
    bool isUnmergedDirectAtomicOrStore =
        (!S->isMerged()) && S->isDirectMemStream() &&
        (S->isAtomicComputeStream() || S->isStoreComputeStream());
    if (!S->isDirectLoadStream() && !S->isPointerChaseLoadStream() &&
        !isUnmergedDirectAtomicOrStore) {
      return FloatDecision();
    }
  }
  /**
   * Make sure we do not offload empty stream.
   * This information may be known at configuration time, or even require
   * oracle information. However, as the stream is empty, trace-based
   * simulation does not know which LLC bank should the stream be offloaded
   * to.
   * TODO: Improve this.
   */
  if (S->se->isTraceSim()) {
    if (S->getStreamLengthAtInstance(dynS.dynamicStreamId.streamInstance) ==
        0) {
      return FloatDecision();
    }
  }

  switch (this->policy) {
  case PolicyE::STATIC:
    return FloatDecision();
  case PolicyE::MANUAL: {
    return this->shouldFloatStreamManual(dynS);
  }
  case PolicyE::SMART_COMPUTATION:
  case PolicyE::SMART: {
    return this->shouldFloatStreamSmart(dynS);
  }
  default: {
    return FloatDecision();
  }
  }

  // Let's use the previous staistic of the average stream.
  bool enableSmartDecision = false;
  if (enableSmartDecision) {
    const auto &statistic = S->statistic;
    if (statistic.numConfigured == 0) {
      // First time, maybe we aggressively offload as this is the
      // case for many microbenchmark we designed.
      return true;
    }
    auto avgLength = statistic.numUsed / statistic.numConfigured;
    if (avgLength < 500) {
      return false;
    }
  }
}

StreamFloatPolicy::FloatDecision
StreamFloatPolicy::shouldFloatStreamManual(DynamicStream &dynS) {
  /**
   * TODO: Really should be a hint in the stream configuration provided by the
   * compiler.
   */
  auto S = dynS.stream;
  static std::unordered_map<Stream *, bool> memorizedDecision;
  auto iter = memorizedDecision.find(S);
  if (iter == memorizedDecision.end()) {
    auto shouldFloat = S->getFloatManual();
    iter = memorizedDecision.emplace(S, shouldFloat).first;
  }

  /**
   * So far manually will always float to L2.
   */
  return FloatDecision(iter->second, MachineType::MachineType_L2Cache);
}

bool StreamFloatPolicy::checkReuseWithinStream(DynamicStream &dynS) {
  auto S = dynS.stream;
  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(S->getAddrGenCallback());
  if (!linearAddrGen) {
    // Non linear addr gen.
    return true;
  }
  auto elementSize = S->getMemElementSize();
  uint64_t reuseFootprint;
  uint64_t reuseCount;
  auto hasReuse = linearAddrGen->estimateReuse(
      dynS.addrGenFormalParams, elementSize, reuseFootprint, reuseCount);
  if (!hasReuse) {
    // No reuse found;
    return true;
  }
  auto privateCacheSize = this->getPrivateCacheCapacity();
  if (reuseFootprint >= privateCacheSize) {
    S_DPRINTF(S, "ReuseSize %lu ReuseCount %d >= PrivateCacheSize %lu.\n",
              reuseFootprint, reuseCount, privateCacheSize);
    logStream(S) << "ReuseSize " << reuseFootprint << " ReuseCount "
                 << reuseCount << " >= PrivateCacheSize " << privateCacheSize
                 << '\n'
                 << std::flush;
    return true;
  } else {
    S_DPRINTF(
        S, "[Not Float] ReuseSize %lu ReuseCount %d < PrivateCacheSize %lu.\n",
        reuseFootprint, reuseCount, privateCacheSize);
    logStream(S) << "[Not Float] ReuseSize " << reuseFootprint << " ReuseCount "
                 << reuseCount << " < PrivateCacheSize " << privateCacheSize
                 << '\n'
                 << std::flush;
    return false;
  }
}

bool StreamFloatPolicy::checkAggregateHistory(DynamicStream &dynS) {
  /**
   * 2. Check if the start address is same as previous configuration, and the
   * previous trip count is short enough to fit in the private cache level.
   * If so, we should not float this stream.
   */
  auto S = dynS.stream;
  if (S->aggregateHistory.empty()) {
    return true;
  }
  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(S->getAddrGenCallback());
  if (!linearAddrGen) {
    // Non linear addr gen.
    return true;
  }
  int historyOffset = -1;
  uint64_t historyTotalElements = 0;
  uint64_t historyStartVAddrMin = UINT64_MAX;
  uint64_t historyStartVAddrMax = 0;
  auto currStartAddr = linearAddrGen->getStartAddr(dynS.addrGenFormalParams);
  logStream(S) << "StartVAddr " << std::hex << currStartAddr << std::dec << '\n'
               << std::flush;
  for (auto historyIter = S->aggregateHistory.rbegin(),
            historyEnd = S->aggregateHistory.rend();
       historyIter != historyEnd; ++historyIter, --historyOffset) {
    const auto &prevHistory = *historyIter;
    auto prevStartAddr = prevHistory.startVAddr;
    auto prevNumElements = prevHistory.numReleasedElements;

    historyTotalElements += prevNumElements;
    historyStartVAddrMax = std::max(historyStartVAddrMax, prevStartAddr);
    historyStartVAddrMin = std::min(historyStartVAddrMin, prevStartAddr);
    logStream(S) << "Hist " << historyOffset << " StartAddr " << std::hex
                 << prevStartAddr << " Range " << historyStartVAddrMin << ", +"
                 << historyStartVAddrMax - historyStartVAddrMin << std::dec
                 << " NumElem " << prevNumElements << '\n'
                 << std::flush;

    // Check if previous stream has more than 50% chance of hit in private
    // cache?
    auto prevIssuedRequests = prevHistory.numIssuedRequests;
    auto prevPrivateCacheHits = prevHistory.numPrivateCacheHits;
    auto prevPrivateCacheHitRate = static_cast<float>(prevPrivateCacheHits) /
                                   static_cast<float>(prevIssuedRequests);
    if (prevPrivateCacheHitRate > 0.5f) {
      // Hit rate too high.
      S_DPRINTF(S,
                "[Not Float] Hist PrevIssued %llu, PrivateCacheHitRate %f.\n",
                prevIssuedRequests, prevPrivateCacheHitRate);
      logStream(S) << "[Not Float] Hist PrevIssued " << prevIssuedRequests
                   << " PrivateCacheHitRate " << prevPrivateCacheHitRate << '\n'
                   << std::flush;
      return false;
    }

    if (currStartAddr != prevStartAddr) {
      // Not match.
      continue;
    }
    // Make sure that the stream is short.
    auto cacheLineSize = S->getCPUDelegator()->cacheLineSize();
    auto memoryFootprint = cacheLineSize * prevHistory.numIssuedRequests;
    auto privateCacheSize = this->getPrivateCacheCapacity();
    if (memoryFootprint > privateCacheSize) {
      // Still should be offloaded.
      S_DPRINTF(S, "Hist %d MemFootPrint %#x > PrivateCache %#x.\n",
                historyOffset, memoryFootprint, privateCacheSize);
      logStream(S) << "Hist " << historyOffset << " MemFootPrint" << std::hex
                   << memoryFootprint << " > PrivateCache " << privateCacheSize
                   << '\n'
                   << std::dec << std::flush;
      continue;
    }
    if (S->addrDepStreams.size() > 0) {
      uint64_t maxIndSMemFootprint = 0;
      for (auto indS : S->addrDepStreams) {
        auto indSMemElementSize = indS->getMemElementSize();
        auto indSMemFootprint =
            indSMemElementSize * prevHistory.numReleasedElements;
        maxIndSMemFootprint = std::max(maxIndSMemFootprint, indSMemFootprint);
      }
      if (maxIndSMemFootprint > privateCacheSize) {
        // If we have indirect streams, then the half the threashold.
        S_DPRINTF(S, "Hist %d MaxIndSMemFootPrint %#x > PrivateCache %#x.\n",
                  historyOffset, maxIndSMemFootprint, privateCacheSize);
        logStream(S) << "Hist " << historyOffset << " MaxMemFootPrint "
                     << std::hex << maxIndSMemFootprint << " > PrivateCache "
                     << privateCacheSize << ".\n"
                     << std::dec << std::flush;
        continue;
      }
    }
    S_DPRINTF(S,
              "[Not Float] Hist %d StartAddr %#x matched, MemFootPrint %lu <= "
              "PrivateCache %lu.\n",
              historyOffset, currStartAddr, memoryFootprint, privateCacheSize);
    logStream(S) << "[Not Float] Hist " << historyOffset << " StartAddr "
                 << std::hex << currStartAddr << " matched, MemFootPrint "
                 << memoryFootprint << " <= PrivateCache " << privateCacheSize
                 << ".\n"
                 << std::dec << std::flush;
    return false;
  }

  /**
   * If the streams are very short (<5), and all start addresses are from
   * a narrow range (currently half of the private L2 size), then we
   * do not float it.
   */
  const uint64_t NUM_ELEMENTS_THRESHOLD = 5;
  const uint64_t START_ADDR_RANGE_MULTIPLIER = 2;
  if (historyTotalElements <
      NUM_ELEMENTS_THRESHOLD * S->aggregateHistory.size()) {
    auto historyStartVAddrRange = historyStartVAddrMax - historyStartVAddrMin;
    if (historyStartVAddrRange * START_ADDR_RANGE_MULTIPLIER <=
        this->getPrivateCacheCapacity()) {
      logStream(S) << "[Not Float] Hist TotalElements " << historyTotalElements
                   << " StartVAddr Range " << historyStartVAddrRange << ".\n"
                   << std::flush;
      return false;
    }
  }

  return true;
}

StreamFloatPolicy::FloatDecision
StreamFloatPolicy::shouldFloatStreamSmart(DynamicStream &dynS) {
  /**
   * 1. Check if there are aliased store stream.
   */
  auto S = dynS.stream;
  if (S->isLoadStream() && S->aliasBaseStream->hasAliasedStoreStream) {
    // Unless I have been promoted as an UpdateStream.
    if (!S->isUpdateStream()) {
      S_DPRINTF(S, "[Not Float] due to aliased store stream.\n");
      logStream(S) << "[Not Float] due to aliased store stream.\n"
                   << std::flush;
      return FloatDecision();
    }
  }

  /**
   * As an experimental feature, we always offload streams with value
   * dependence, and store streams with store func, as these are targets for
   * computation offloading.
   */
  if (this->policy == PolicyE::SMART_COMPUTATION) {
    bool floatCompute = false;
    if (!S->valueDepStreams.empty() || S->getEnabledStoreFunc() ||
        S->getEnabledLoadFunc()) {
      floatCompute = true;
    }
    for (auto depS : S->addrDepStreams) {
      if (depS->getEnabledStoreFunc() || depS->getEnabledLoadFunc()) {
        floatCompute = true;
      }
    }
    if (floatCompute) {
      auto machineType = this->chooseFloatMachineType(dynS);
      S_DPRINTF(S, "[Float] %s always float computation.", machineType);
      logStream(S) << "[Float] " << machineType
                   << " always float computation.\n"
                   << std::flush;
      return FloatDecision(true, machineType);
    }
  }

  if (!this->checkReuseWithinStream(dynS)) {
    return FloatDecision(false);
  }

  if (!this->checkAggregateHistory(dynS)) {
    return FloatDecision(false);
  }

  if (S->getStreamName() ==
          "(kernel_query.c::30(.omp_outlined..33) 50 bb87 bb87::tmp91(load))" ||
      S->getStreamName() == "(kernel_range.c::28(.omp_outlined..37) 62 bb104 "
                            "bb104::tmp109(load))" ||
      S->getStreamName() == "(kernel_range.c::28(.omp_outlined..37) 67 bb104 "
                            "bb118::tmp121(load))") {
    logStream(S) << "[NotFloated]: explicitly.\n" << std::flush;
    return FloatDecision(false);
  }

  auto machineType = this->chooseFloatMachineType(dynS);
  S_DPRINTF(S, "[Float] %s.\n", machineType);
  logStream(S) << "[Float] " << machineType << ".\n" << std::flush;
  return FloatDecision(true, machineType);
}

bool StreamFloatPolicy::shouldPseudoFloatStream(DynamicStream &dynS) {
  /**
   * So far we use simple heuristic:
   * 1. It has indirect streams.
   * 2. Its TotalTripCount is known and shorter than a threshold.
   * 3. TODO: Use history hit information.
   */
  auto S = dynS.stream;
  if (S->addrDepStreams.empty()) {
    return false;
  }
  if (!dynS.hasTotalTripCount()) {
    return false;
  }
  auto totalTripCount = dynS.getTotalTripCount();
  constexpr int MaxTotalTripCount = 10;
  if (totalTripCount > MaxTotalTripCount) {
    return false;
  }
  S_DPRINTF(S, "[PseudoFloat] TotalTripCount %lu.\n", totalTripCount);
  logStream(S) << "[PseudoFloat] TotalTripCount " << totalTripCount << '\n'
               << std::flush;
  return true;
}

MachineType StreamFloatPolicy::chooseFloatMachineType(DynamicStream &dynS) {
  if (!this->enabledFloatMem) {
    // By default we float to L2 cache (LLC in MESI_Three_Level).
    return MachineType::MachineType_L2Cache;
  }

  /**
   * Static policy will always float to memory.
   * Smart policy will try to analyze the reuse.
   */
  if (this->levelPolicy == LevelPolicyE::LEVEL_STATIC) {
    return MachineType::MachineType_Directory;
  } else if (this->levelPolicy == LevelPolicyE::LEVEL_MANUAL) {
    return this->chooseFloatMachineTypeManual(dynS);
  }

  auto S = dynS.stream;
  if (S->aggregateHistory.size() < 2) {
    return MachineType::MachineType_L2Cache;
  }
  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(S->getAddrGenCallback());
  if (!linearAddrGen) {
    // Non linear addr gen.
    return MachineType::MachineType_Directory;
  }
  int historyOffset = -1;
  uint64_t historyTotalElements = 0;
  uint64_t historyStartVAddrMin = UINT64_MAX;
  uint64_t historyStartVAddrMax = 0;
  auto currStartAddr = linearAddrGen->getStartAddr(dynS.addrGenFormalParams);
  logStream(S) << "StartVAddr " << std::hex << currStartAddr << std::dec << '\n'
               << std::flush;
  for (auto historyIter = S->aggregateHistory.rbegin(),
            historyEnd = S->aggregateHistory.rend();
       historyIter != historyEnd; ++historyIter, --historyOffset) {
    const auto &prevHistory = *historyIter;
    auto prevStartAddr = prevHistory.startVAddr;
    auto prevNumElements = prevHistory.numReleasedElements;

    historyTotalElements += prevNumElements;
    historyStartVAddrMax = std::max(historyStartVAddrMax, prevStartAddr);
    historyStartVAddrMin = std::min(historyStartVAddrMin, prevStartAddr);
    logStream(S) << "Hist " << historyOffset << " StartAddr " << std::hex
                 << prevStartAddr << " Range " << historyStartVAddrMin << ", +"
                 << historyStartVAddrMax - historyStartVAddrMin << std::dec
                 << " NumElem " << prevNumElements << '\n'
                 << std::flush;

    if (currStartAddr != prevStartAddr) {
      // Not match.
      continue;
    }
    // Make sure that the stream is short.
    auto memoryFootprint =
        S->getMemElementSize() * prevHistory.numReleasedElements;
    auto sharedCacheSize = this->getSharedLLCCapacity();
    if (memoryFootprint >= sharedCacheSize) {
      // Still should be offloaded to Memory.
      S_DPRINTF(S, "Hist %d MemFootPrint %#x > SharedCache %#x.\n",
                historyOffset, memoryFootprint, sharedCacheSize);
      logStream(S) << "Hist " << historyOffset << " MemFootPrint" << std::hex
                   << memoryFootprint << " > SharedCache " << sharedCacheSize
                   << '\n'
                   << std::dec << std::flush;
      continue;
    }
    S_DPRINTF(S,
              "[TryFitLLC] Hist %d StartAddr %#x matched, MemFootPrint %lu <= "
              "SharedCache %lu.\n",
              historyOffset, currStartAddr, memoryFootprint, sharedCacheSize);
    logStream(S) << "[TryFitLLC] Hist " << historyOffset << " StartAddr "
                 << std::hex << currStartAddr << " matched, MemFootPrint "
                 << memoryFootprint << " <= SharedCache " << sharedCacheSize
                 << ".\n"
                 << std::dec << std::flush;
    return MachineType::MachineType_L2Cache;
  }

  return MachineType::MachineType_Directory;
}

MachineType
StreamFloatPolicy::chooseFloatMachineTypeManual(DynamicStream &dynS) {

  /**
   * Manually check for the stream name.
   * Default to L2 cache.
   */
  auto S = dynS.stream;
  auto iter = this->memorizedManualFloatMachineType.find(S);
  if (iter == this->memorizedManualFloatMachineType.end()) {

    static const std::unordered_set<std::string> manualFloatToMemSet = {
        "rodinia.pathfinder.wall.ld",
    };

    MachineType floatToMachine = MachineType::MachineType_L2Cache;
    if (manualFloatToMemSet.count(S->getStreamName())) {
      floatToMachine = MachineType::MachineType_Directory;
    }

    iter =
        this->memorizedManualFloatMachineType.emplace(S, floatToMachine).first;
  }

  S_DPRINTF(S, "[Level] Manually Float to %s.\n", iter->second);
  logStream(S) << "[Level] Manually Float to " << iter->second << "\n"
               << std::dec << std::flush;
  return iter->second;
}