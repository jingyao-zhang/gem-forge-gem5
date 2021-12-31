
#include "stream_float_policy.hh"

#include "mem/ruby/structures/CacheMemory.hh"
#include "sim/stream_nuca/stream_nuca_manager.hh"
#include "stream_engine.hh"

#include "debug/StreamFloatPolicy.hh"
#define DEBUG_TYPE StreamFloatPolicy
#include "stream_log.hh"

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

void StreamFloatPolicy::setFloatPlanManual(DynamicStream &dynS) {

  /**
   * Manually check for the stream name.
   * Default to L2 cache.
   */
  auto S = dynS.stream;
  const auto &streamName = S->getStreamName();

  auto &floatPlan = dynS.getFloatPlan();
  uint64_t firstElementIdx = 0;

  static const std::unordered_set<std::string> manualFloatToMemSet = {
      "rodinia.pathfinder.wall.ld", "rodinia.hotspot.power.ld",
      "rodinia.hotspot3D.power.ld", "gap.pr_push.atomic.out_v.ld",
      "gap.bfs_push.out_v.ld",      "gap.sssp.out_v.ld",
      "gap.sssp.out_w.ld",          "gap.pr_pull.acc.in_v.ld",
      "gap.bfs_pull.in_v.ld",
  };

  if (manualFloatToMemSet.count(streamName)) {
    floatPlan.addFloatChangePoint(firstElementIdx, MachineType_Directory);
    return;
  }

  if (streamName.find("rodinia.srad_v2.") == 0) {
    /**
     * For srad_v2, we want to split them at iterations.
     */
    if (!dynS.hasTotalTripCount()) {
      DYN_S_PANIC(dynS.dynamicStreamId,
                  "Missing TotalTripCount for rodinia.srad_v2.");
    }

    auto linearAddrGen =
        std::dynamic_pointer_cast<LinearAddrGenCallback>(dynS.addrGenCallback);
    if (!linearAddrGen) {
      // They should have linear address pattern.
      DYN_S_PANIC(dynS.dynamicStreamId,
                  "Non-LinearAddrGen for rodinia.srad_v2.");
    }

    auto totalTripCount = dynS.getTotalTripCount();
    auto rowTripCount =
        linearAddrGen->getNestTripCount(dynS.addrGenFormalParams, 1);

    // Take min to handle the coalesced stream.
    auto elementSize = std::min(S->getMemElementSize(), 64);
    auto totalThreads =
        S->getCPUDelegator()->getSingleThreadContext()->getThreadGroupSize();
    auto totalArrays = 6;

    auto totalLLCBytes = this->getSharedLLCCapacity();
    auto myLLCBytes = totalLLCBytes / totalThreads;
    auto rowDataBytes = rowTripCount * elementSize * totalArrays;
    auto myLLCRows = myLLCBytes / rowDataBytes;

    auto myRows = totalTripCount / rowTripCount;
    hack("TotalTripCount %d RowTripCount %d MyRows %d MyLLCRows %d.\n",
         totalTripCount, rowTripCount, myRows, myLLCRows);

    if (myRows <= myLLCRows) {
      // I should be able to fit in LLC.
      floatPlan.addFloatChangePoint(firstElementIdx, MachineType_L2Cache);
      return;
    }

    /**
     * For now we start with LLC and then migrate to Mem.
     * And we take care of reuse across rows.
     */
    if (streamName == "rodinia.srad_v2.Jn.ld") {
      myLLCRows++;
    } else if (streamName == "rodinia.srad_v2.Js.ld") {
      myLLCRows--;
    } else if (streamName == "rodinia.srad_v2.cN.ld") {
      myLLCRows++;
    } else if (streamName == "rodinia.srad_v2.cS.ld") {
      myLLCRows--;
    }
    auto llcTripCount = myLLCRows * rowTripCount;
    floatPlan.addFloatChangePoint(firstElementIdx, MachineType_L2Cache);
    floatPlan.addFloatChangePoint(llcTripCount, MachineType_Directory);
    return;
  }

  // Default just offload to LLC.
  floatPlan.addFloatChangePoint(firstElementIdx, MachineType_L2Cache);
  return;
}

void StreamFloatPolicy::setFloatPlanManual2(DynamicStream &dynS) {

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
    floatPlan.addFloatChangePoint(firstElementIdx, MachineType_Directory);
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
    if (region.cachedElements == 0) {
      DYN_S_DPRINTF(dynS.dynamicStreamId,
                    "Directly float to Mem as zero cached elements.\n");
      floatPlan.addFloatChangePoint(firstElementIdx, MachineType_Directory);
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
  if (streamName.find("gfm.bin_tree.val.ld") == 0 ||
      (streamName.find("(omp_binary_tree.c") == 0 && S->getLoopLevel() == 2)) {
    this->setFloatPlanForBinTree(dynS);
    return;
  }

  // Default just offload to LLC.
  floatPlan.addFloatChangePoint(firstElementIdx, MachineType_L2Cache);
  return;
}

void StreamFloatPolicy::setFloatPlanForRodiniaSrad(DynamicStream &dynS) {

  if (!dynS.hasTotalTripCount()) {
    DYN_S_PANIC(dynS.dynamicStreamId,
                "Missing TotalTripCount for iter-based floating plan..");
  }

  auto S = dynS.stream;

  auto &floatPlan = dynS.getFloatPlan();
  uint64_t firstElementIdx = 0;

  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(dynS.addrGenCallback);
  if (!linearAddrGen) {
    // They should have linear address pattern.
    DYN_S_PANIC(dynS.dynamicStreamId,
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

  DYN_S_DPRINTF(dynS.dynamicStreamId,
                "TotalTripCount %d LLCEndVAddr %#x = %#x + %lu * %lu  "
                "MyEndVAddr %#x = %#x + %d MyEnd %s LLCEnd.\n",
                totalTripCount, llcEndVAddr, streamNUCARegion.vaddr,
                streamNUCARegion.cachedElements, streamNUCARegion.elementSize,
                myEndVAddr, myStartVAddr, totalTripCount * elementSize,
                myEndVAddr < llcEndVAddr ? "<" : ">=");

  if (myEndVAddr <= llcEndVAddr) {
    // We are accessing rows cached in LLC.
    floatPlan.addFloatChangePoint(firstElementIdx, MachineType_L2Cache);
    return;
  }

  if (myStartVAddr >= llcEndVAddr) {
    // We accessing rows not cached in LLC.
    floatPlan.addFloatChangePoint(firstElementIdx, MachineType_Directory);
    return;
  }

  /**
   * We are accessing mixing rows in LLC and Mem.
   * Here we add (elementSize - 1) to handle cross-line element.
   */
  auto myLLCTripCount =
      (llcEndVAddr - myStartVAddr + elementSize - 1) / elementSize;
  floatPlan.addFloatChangePoint(firstElementIdx, MachineType_L2Cache);
  floatPlan.addFloatChangePoint(myLLCTripCount, MachineType_Directory);
  return;
}

void StreamFloatPolicy::setFloatPlanForBinTree(DynamicStream &dynS) {

  auto S = dynS.stream;

  auto &floatPlan = dynS.getFloatPlan();
  uint64_t firstElementIdx = 0;

  auto threadContext = S->getCPUDelegator()->getSingleThreadContext();
  auto streamNUCAManager = threadContext->getStreamNUCAManager();

  const auto &streamNUCARegion =
      streamNUCAManager->getRegionFromName("gfm.bin_tree.tree");

  auto cachedElements = streamNUCARegion.cachedElements;
  auto logCachedElements = static_cast<int>(log2(cachedElements));

  DYN_S_DPRINTF(dynS.dynamicStreamId,
                "ElemSize %d CachedElements %lu LogCachedElements %d.\n",
                streamNUCARegion.elementSize, cachedElements,
                logCachedElements);
  logS(dynS) << "[BinTree] ElemSize " << streamNUCARegion.elementSize
             << " CachedElements " << cachedElements << " LogCachedElements "
             << logCachedElements << ".\n"
             << std::flush;
  /**
   * We are accessing mixing rows in LLC and Mem.
   * Here we add (elementSize - 1) to handle cross-line element.
   */
  floatPlan.addFloatChangePoint(firstElementIdx, MachineType_L2Cache);
  floatPlan.addFloatChangePoint(logCachedElements, MachineType_Directory);
  return;
}