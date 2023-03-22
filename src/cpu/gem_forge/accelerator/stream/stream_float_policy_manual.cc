
#include "stream_float_policy.hh"

#include "mem/ruby/structures/CacheMemory.hh"
#include "sim/stream_nuca/stream_nuca_manager.hh"
#include "stream_engine.hh"

#include "debug/StreamFloatPolicy.hh"
#define DEBUG_TYPE StreamFloatPolicy
#include "stream_log.hh"

namespace gem5 {

const std::unordered_map<std::string, std::string>
    StreamFloatPolicy::streamToRegionMap = {
        {"rodinia.srad_v2.Jc.ld", "rodinia.srad_v2.J"},
        {"rodinia.srad_v2.Jw.ld", "rodinia.srad_v2.J"},
        {"rodinia.srad_v2.Je.ld", "rodinia.srad_v2.J"},
        {"rodinia.srad_v2.Jn.ld", "rodinia.srad_v2.J"},
        {"rodinia.srad_v2.Js.ld", "rodinia.srad_v2.J"},
        {"rodinia.srad_v2.c.st", "rodinia.srad_v2.c"},
        {"rodinia.srad_v2.deltaN.st", "rodinia.srad_v2.deltaN"},
        {"rodinia.srad_v2.deltaS.st", "rodinia.srad_v2.deltaS"},
        {"rodinia.srad_v2.deltaE.st", "rodinia.srad_v2.deltaE"},
        {"rodinia.srad_v2.deltaW.st", "rodinia.srad_v2.deltaW"},
        {"rodinia.srad_v2.cN.ld", "rodinia.srad_v2.c"},
        {"rodinia.srad_v2.cS.ld", "rodinia.srad_v2.c"},
        {"rodinia.srad_v2.cE.ld", "rodinia.srad_v2.c"},
        {"rodinia.srad_v2.deltaN.ld", "rodinia.srad_v2.deltaN"},
        {"rodinia.srad_v2.deltaS.ld", "rodinia.srad_v2.deltaS"},
        {"rodinia.srad_v2.deltaE.ld", "rodinia.srad_v2.deltaE"},
        {"rodinia.srad_v2.deltaW.ld", "rodinia.srad_v2.deltaW"},
        {"rodinia.srad_v2.Jc2.ld", "rodinia.srad_v2.J"},
        {"rodinia.srad_v2.J.st", "rodinia.srad_v2.J"},
        {"gap.pr_push.atomic.out_begin.ld", "gap.pr_push.out_neigh_index"},
        {"gap.pr_push.atomic.out_v.ld", "gap.pr_push.out_edge"},
        {"gap.bfs_push.out_begin.ld", "gap.bfs_push.out_neigh_index"},
        {"gap.bfs_push.out_v.ld", "gap.bfs_push.out_edge"},
};

void StreamFloatPolicy::setFloatPlanManual(DynStream &dynS) {

  /**
   * Manually check for the stream name.
   * Default to L2 cache.
   */
  auto S = dynS.stream;
  const auto &streamName = S->getStreamName();

  auto &floatPlan = dynS.getFloatPlan();
  uint64_t firstElementIdx = 0;

  static const std::unordered_set<std::string> manualFloatToMemSet = {
      "gap.pr_push.atomic.out_v.ld", "gap.bfs_push.out_v.ld",
      "gap.sssp.out_v.ld",           "gap.sssp.out_w.ld",
      "gap.pr_pull.acc.in_v.ld",     "gap.bfs_pull.in_v.ld",
  };

  if (manualFloatToMemSet.count(streamName)) {
    if (this->enabledFloatMem) {
      floatPlan.addFloatChangePoint(firstElementIdx,
                                    ruby::MachineType_Directory);
    } else {
      // By default we float to L2 cache (LLC in MESI_Three_Level).
      floatPlan.addFloatChangePoint(firstElementIdx, ruby::MachineType_L2Cache);
    }
    return;
  }

  /**
   * Or if the stream's region is not cached at all.
   */
  if (streamToRegionMap.count(streamName)) {
    const auto &regionName = streamToRegionMap.at(streamName);
    auto tc = S->getCPUDelegator()->getSingleThreadContext();
    auto process = tc->getProcessPtr();
    auto streamNUCAManager = process->streamNUCAManager;
    const auto &region = streamNUCAManager->getRegionFromName(regionName);
    if (region.cachedElements == 0 && this->enabledFloatMem) {
      DYN_S_DPRINTF(dynS.dynStreamId,
                    "Directly float to Mem as zero cached elements.\n");
      floatPlan.addFloatChangePoint(firstElementIdx,
                                    ruby::MachineType_Directory);
      return;
    }
  }

  /**
   * Split streams at iterations:
   * rodinia.srad_v2/v3
   * rodinia.hotspot
   */
  if (streamName.find("rodinia.srad_v2.") == 0 ||
      streamName.find("rodinia.srad_v3.") == 0 ||
      streamName.find("rodinia.hotspot.") == 0 ||
      streamName.find("rodinia.hotspot3D.") == 0 ||
      streamName.find("rodinia.pathfinder.") == 0) {
    this->setFloatPlanForRodiniaSrad(dynS);
    return;
  }

  /**
   * Take log for binary tree.
   */
  if (streamName.find("gfm.bin_tree.val.ld") != std::string::npos ||
      (streamName.find("(omp_binary_tree.c") != std::string::npos &&
       S->getLoopLevel() == 2)) {
    this->setFloatPlanForBinTree(dynS);
    return;
  }

  // Default just offload to LLC.
  floatPlan.addFloatChangePoint(firstElementIdx, ruby::MachineType_L2Cache);
  return;
}

void StreamFloatPolicy::setFloatPlanForRodiniaSrad(DynStream &dynS) {

  if (!dynS.hasTotalTripCount()) {
    DYN_S_PANIC(dynS.dynStreamId,
                "Missing TotalTripCount for iter-based floating plan..");
  }

  auto S = dynS.stream;

  auto &floatPlan = dynS.getFloatPlan();
  uint64_t firstElementIdx = 0;

  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(dynS.addrGenCallback);
  if (!linearAddrGen) {
    // They should have linear address pattern.
    DYN_S_PANIC(dynS.dynStreamId,
                "Non-LinearAddrGen for iter-based floating plan.");
  }

  auto totalTripCount = dynS.getTotalTripCount();

  // Take min to handle the coalesced stream.
  auto elementSize = std::min(S->getMemElementSize(), 64);

  auto myStartVAddr = linearAddrGen->getStartAddr(dynS.addrGenFormalParams);
  // ! This only considers the case when the address pattern is increasing.
  auto myEndVAddr = myStartVAddr + totalTripCount * elementSize;

  auto threadContext = S->getCPUDelegator()->getSingleThreadContext();
  auto streamNUCAManager = threadContext->getStreamNUCAManager();

  const auto &streamNUCARegion =
      streamNUCAManager->getContainingStreamRegion(myStartVAddr);

  auto cachedBytes =
      streamNUCARegion.cachedElements * streamNUCARegion.elementSize;
  auto llcEndVAddr = streamNUCARegion.vaddr + cachedBytes;

  DYN_S_DPRINTF(dynS.dynStreamId,
                "TripCount %d LLCEndVAddr %#x = %#x + %lu * %lu  "
                "MyEndVAddr %#x = %#x + %d MyEnd %s LLCEnd.\n",
                totalTripCount, llcEndVAddr, streamNUCARegion.vaddr,
                streamNUCARegion.cachedElements, streamNUCARegion.elementSize,
                myEndVAddr, myStartVAddr, totalTripCount * elementSize,
                myEndVAddr < llcEndVAddr ? "<" : ">=");

  if (myEndVAddr <= llcEndVAddr) {
    // We are accessing rows cached in LLC.
    floatPlan.addFloatChangePoint(firstElementIdx, ruby::MachineType_L2Cache);
    return;
  }

  if (myStartVAddr >= llcEndVAddr) {
    // We accessing rows not cached in LLC.
    if (this->enabledFloatMem) {
      floatPlan.addFloatChangePoint(firstElementIdx,
                                    ruby::MachineType_Directory);
    } else {
      floatPlan.addFloatChangePoint(firstElementIdx, ruby::MachineType_L2Cache);
    }
    return;
  }

  /**
   * We are accessing mixing rows in LLC and Mem.
   * Here we add (elementSize - 1) to handle cross-line element.
   */
  auto myLLCTripCount =
      (llcEndVAddr - myStartVAddr + elementSize - 1) / elementSize;
  floatPlan.addFloatChangePoint(firstElementIdx, ruby::MachineType_L2Cache);
  if (this->enabledFloatMem) {
    floatPlan.addFloatChangePoint(myLLCTripCount, ruby::MachineType_Directory);
  }
  return;
}

void StreamFloatPolicy::setFloatPlanForBinTree(DynStream &dynS) {

  auto S = dynS.stream;

  auto &floatPlan = dynS.getFloatPlan();

  auto threadContext = S->getCPUDelegator()->getSingleThreadContext();
  auto streamNUCAManager = threadContext->getStreamNUCAManager();

  // We have some initial guess.
  // We minus 1 to avoid overwhelming the cache.
  int64_t cachedElems = 2 * 1024 * 1024 - 1;
  int64_t elemSize = 64;
  if (const auto *treeRegion =
          streamNUCAManager->tryGetRegionFromName("gfm.bin_tree.tree")) {
    cachedElems = treeRegion->cachedElements - 1;
    elemSize = treeRegion->elementSize;
  }

  auto logCachedElems = static_cast<int>(log2(cachedElems));

  auto privateCacheSize = this->getPrivateCacheCapacity();
  auto privCachedElems = privateCacheSize / elemSize - 1;
  auto logPrivCachedElems = static_cast<int>(log2(privCachedElems));

  // Override the decision if user specified.
  {
    auto userSetMidwayElemIdx =
        S->se->params().streamEngineMidwayFloatElementIdx;
    if (userSetMidwayElemIdx != -1) {
      logPrivCachedElems = userSetMidwayElemIdx;
    }
  }

  DYN_S_DPRINTF(
      dynS.dynStreamId,
      "ElemSize %d PrivCached %lu LogPrivCached %d Cached %lu LogCached %d.\n",
      elemSize, privCachedElems, logPrivCachedElems, cachedElems,
      logCachedElems);
  logS(dynS) << "[BinTree] ElemSize " << elemSize << " PrivCached "
             << privCachedElems << " PrivLogCached " << logPrivCachedElems
             << " Cached " << cachedElems << " LogCached " << logCachedElems
             << ".\n"
             << std::flush;

  /**
   * We start from core to LLC to mem.
   */
  dynS.setNextCacheDoneElemIdx(logPrivCachedElems);
  if (logPrivCachedElems > 0) {
    floatPlan.addFloatChangePoint(0, ruby::MachineType_NULL);
  }
  floatPlan.addFloatChangePoint(logPrivCachedElems, ruby::MachineType_L2Cache);
  if (this->enabledFloatMem) {
    floatPlan.addFloatChangePoint(logCachedElems, ruby::MachineType_Directory);
  }
  return;
}
} // namespace gem5
