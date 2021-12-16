#include "stream_nuca_manager.hh"
#include "numa_page_allocator.hh"
#include "stream_nuca_map.hh"

#include "base/trace.hh"
#include "cpu/thread_context.hh"

#include <iomanip>
#include <unordered_set>

#include "debug/StreamNUCAManager.hh"

bool StreamNUCAManager::statsRegsiterd = false;
Stats::ScalarNoReset StreamNUCAManager::indRegionPages;
Stats::ScalarNoReset StreamNUCAManager::indRegionElements;
Stats::ScalarNoReset StreamNUCAManager::indRegionAllocPages;
Stats::ScalarNoReset StreamNUCAManager::indRegionRemapPages;
Stats::ScalarNoReset StreamNUCAManager::indRegionMemToLLCDefaultHops;
Stats::ScalarNoReset StreamNUCAManager::indRegionMemToLLCMinHops;
Stats::DistributionNoReset StreamNUCAManager::indRegionMemMinBanks;
Stats::ScalarNoReset StreamNUCAManager::indRegionMemToLLCRemappedHops;
Stats::DistributionNoReset StreamNUCAManager::indRegionMemRemappedBanks;

StreamNUCAManager::StreamNUCAManager(Process *_process, bool _enabled,
                                     const std::string &_directRegionFitPolicy,
                                     bool _enableIndirectPageRemap)
    : process(_process), enabled(_enabled),
      enableIndirectPageRemap(_enableIndirectPageRemap) {
  if (_directRegionFitPolicy == "crop") {
    this->directRegionFitPolicy = DirectRegionFitPolicy::CROP;
  } else if (_directRegionFitPolicy == "drop") {
    this->directRegionFitPolicy = DirectRegionFitPolicy::DROP;
  } else {
    panic("Unknown DirectRegionFitPolicy %s.", _directRegionFitPolicy);
  }
}

StreamNUCAManager::StreamNUCAManager(const StreamNUCAManager &other)
    : process(other.process), enabled(other.enabled),
      directRegionFitPolicy(other.directRegionFitPolicy),
      enableIndirectPageRemap(other.enableIndirectPageRemap) {
  panic("StreamNUCAManager does not have copy constructor.");
}

StreamNUCAManager &
StreamNUCAManager::operator=(const StreamNUCAManager &other) {
  panic("StreamNUCAManager does not have copy constructor.");
}

void StreamNUCAManager::regStats() {

  if (statsRegsiterd) {
    return;
  }
  statsRegsiterd = true;
  hack("Register %#x processor %#x name %s.\n", this, process, process->name());

  assert(this->process && "No process.");

#define scalar(stat, describe)                                                 \
  stat.name(this->process->name() + (".snm." #stat))                           \
      .desc(describe)                                                          \
      .prereq(this->stat)
#define distribution(stat, start, end, step, describe)                         \
  stat.name(this->process->name() + (".snm." #stat))                           \
      .init(start, end, step)                                                  \
      .desc(describe)                                                          \
      .flags(Stats::pdf)

  scalar(indRegionPages, "Pages in indirect region.");
  scalar(indRegionElements, "Elements in indirect region.");
  scalar(indRegionAllocPages,
         "Pages allocated (including fragments) to optimize indirect region.");
  scalar(indRegionRemapPages, "Pages remapped to optimize indirect region.");
  scalar(indRegionMemToLLCDefaultHops,
         "Default hops from Mem to LLC in indirect region.");
  scalar(indRegionMemToLLCMinHops,
         "Minimal hops from Mem to LLC in indirect region.");
  scalar(indRegionMemToLLCRemappedHops,
         "Remapped hops from Mem to LLC in indirect region.");

  auto numMemNodes = StreamNUCAMap::getNUMANodes().size();
  distribution(indRegionMemMinBanks, 0, numMemNodes - 1, 1,
               "Distribution of minimal IndRegion banks.");
  distribution(indRegionMemRemappedBanks, 0, numMemNodes - 1, 1,
               "Distribution of remapped IndRegion banks.");

#undef distribution
#undef scalar
}

void StreamNUCAManager::defineRegion(const std::string &regionName, Addr start,
                                     uint64_t elementSize,
                                     uint64_t numElement) {
  DPRINTF(StreamNUCAManager, "Define Region %s %#x %lu %lu %lukB.\n",
          regionName, start, elementSize, numElement,
          elementSize * numElement / 1024);
  this->startVAddrRegionMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(start),
      std::forward_as_tuple(regionName, start, elementSize, numElement));
}

void StreamNUCAManager::defineAlign(Addr A, Addr B, int64_t elementOffset) {
  DPRINTF(StreamNUCAManager, "Define Align %#x %#x Offset %ld.\n", A, B,
          elementOffset);
  auto &regionA = this->getRegionFromStartVAddr(A);
  regionA.aligns.emplace_back(A, B, elementOffset);
  if (elementOffset < 0) {
    regionA.isIndirect = true;
  }
}

const StreamNUCAManager::StreamRegion &
StreamNUCAManager::getContainingStreamRegion(Addr vaddr) const {
  auto iter = this->startVAddrRegionMap.upper_bound(vaddr);
  if (iter == this->startVAddrRegionMap.begin()) {
    panic("Failed to find ContainingStreamRegion for %#x.", vaddr);
  }
  iter--;
  const auto &region = iter->second;
  if (region.vaddr + region.elementSize * region.numElement <= vaddr) {
    panic("Failed to find ContainingStreamRegion for %#x.", vaddr);
  }
  return region;
}

void StreamNUCAManager::remap(ThreadContext *tc) {
  DPRINTF(StreamNUCAManager, "Remap Regions Enabled %d.\n", this->enabled);
  if (!this->enabled) {
    return;
  }

  bool hasAlign = false;
  for (const auto &entry : this->startVAddrRegionMap) {
    if (!entry.second.aligns.empty()) {
      hasAlign = true;
      break;
    }
  }
  if (!hasAlign) {
    DPRINTF(StreamNUCAManager, "Skip Remapping Region as No Alignments.\n");
  }

  /**
   * We perform a DFS on regions to try to satisfy alignment requirement.
   */
  std::unordered_map<Addr, int> regionRemapStateMap;
  std::vector<Addr> stack;
  while (true) {
    stack.clear();
    for (const auto &entry : this->startVAddrRegionMap) {
      auto regionVAddr = entry.second.vaddr;
      if (regionRemapStateMap.count(regionVAddr) == 0) {
        // We found a unprocessed region.
        regionRemapStateMap.emplace(regionVAddr, 0);
        stack.push_back(regionVAddr);
        break;
      }
    }
    if (stack.empty()) {
      // No region to process.
      break;
    }
    while (!stack.empty()) {
      auto regionVAddr = stack.back();
      auto &region = this->getRegionFromStartVAddr(regionVAddr);
      auto state = regionRemapStateMap.at(regionVAddr);
      if (state == 0) {
        // First time, push AlignToRegions into the stack.
        for (const auto &align : region.aligns) {
          if (align.vaddrB == regionVAddr) {
            // We need to ignore self-alignment.
            continue;
          }
          const auto &alignToRegion =
              this->getRegionFromStartVAddr(align.vaddrB);
          // Check the state of the AlignToRegion.
          auto alignToRegionState =
              regionRemapStateMap.emplace(align.vaddrB, 0).first->second;
          if (alignToRegionState == 0) {
            // The AlignToRegion has not been processed yet.
            stack.push_back(align.vaddrB);
          } else if (alignToRegionState == 1) {
            // The AlignToRegion is on the current DFS path. Must be cycle.
            panic("[StreamNUCA] Cycle in AlignGraph: %s -> %s.", region.name,
                  alignToRegion.name);
          } else {
            // The AlignToRegion has already been processed. Ignore it.
          }
        }
        // Set myself as in stack.
        regionRemapStateMap.at(regionVAddr) = 1;

      } else if (state == 1) {
        // Second time, we can try to remap this region.
        this->remapRegion(tc, region);
        regionRemapStateMap.at(regionVAddr) = 2;
        stack.pop_back();

      } else {
        // This region is already remapped. Ignore it.
        stack.pop_back();
      }
    }
  }

  this->computeCacheSet();

  DPRINTF(StreamNUCAManager,
          "[StreamNUCA] Remap Done. IndRegion: Pages %lu Elements %lu "
          "AllocPages %lu RemapPages %lu DefaultHops %lu MinHops %lu RemapHops "
          "%lu.\n",
          static_cast<uint64_t>(indRegionPages.value()),
          static_cast<uint64_t>(indRegionElements.value()),
          static_cast<uint64_t>(indRegionAllocPages.value()),
          static_cast<uint64_t>(indRegionRemapPages.value()),
          static_cast<uint64_t>(indRegionMemToLLCDefaultHops.value()),
          static_cast<uint64_t>(indRegionMemToLLCMinHops.value()),
          static_cast<uint64_t>(indRegionMemToLLCRemappedHops.value()));
}

void StreamNUCAManager::remapRegion(ThreadContext *tc, StreamRegion &region) {
  bool hasIndirectAlign = false;
  for (const auto &align : region.aligns) {
    if (align.elementOffset < 0) {
      hasIndirectAlign = true;
      break;
    }
  }
  if (hasIndirectAlign) {
    this->remapIndirectRegion(tc, region);
  } else {
    this->remapDirectRegion(region);
  }
}

void StreamNUCAManager::remapDirectRegion(const StreamRegion &region) {
  if (!this->isPAddrContinuous(region)) {
    panic("[StreamNUCA] Region %s %#x PAddr is not continuous.", region.name,
          region.vaddr);
  }
  auto startVAddr = region.vaddr;
  auto startPAddr = this->translate(startVAddr);

  auto endPAddr = startPAddr + region.elementSize * region.numElement;

  uint64_t interleave = this->determineInterleave(region);
  int startBank = this->determineStartBank(region, interleave);
  int startSet = 0;

  StreamNUCAMap::addRangeMap(startPAddr, endPAddr, interleave, startBank,
                             startSet);
  DPRINTF(StreamNUCAManager,
          "[StreamNUCA] Map Region %s %#x PAddr %#x Interleave %lu Bank %d.\n",
          region.name, startVAddr, startPAddr, interleave, startBank);
}

void StreamNUCAManager::remapIndirectRegion(ThreadContext *tc,
                                            StreamRegion &region) {

  /**
   * We divide this into multiple phases:
   * 1. Collect hops stats.
   * 2. Greedily allocate pages to the NUMA Nodes with minimal traffic.
   * 3. If imbalanced, we try to remap.
   * 4. Relocate pages if necessary.
   *
   * NOTE: For now remapped indirect region is not cached.
   */
  region.cachedElements = 0;

  auto regionHops = this->computeIndirectRegionHops(tc, region);

  this->greedyAssignIndirectPages(regionHops);
  this->rebalanceIndirectPages(regionHops);

  this->relocateIndirectPages(tc, regionHops);
}

StreamNUCAManager::IndirectRegionHops
StreamNUCAManager::computeIndirectRegionHops(ThreadContext *tc,
                                             const StreamRegion &region) {
  assert(region.aligns.size() == 1 &&
         "IndirectRegion should have only one align.");
  const auto &align = region.aligns.front();
  assert(align.vaddrB != region.vaddr && "Self-IndirectAlign?");

  /**
   * Scan through the indirect regions and collect hops.
   */
  auto pTable = this->process->pTable;
  auto pageSize = pTable->getPageSize();
  auto totalSize = region.elementSize * region.numElement;
  auto endVAddr = region.vaddr + totalSize;
  if (pTable->pageOffset(region.vaddr) != 0) {
    panic("[StreamNUCA] IndirectRegion %s VAddr %#x should align to page.",
          region.name, region.vaddr);
  }

  const auto &memNodes = StreamNUCAMap::getNUMANodes();
  auto numMemNodes = memNodes.size();
  IndirectRegionHops regionHops(region, numMemNodes);

  const auto &alignToRegion = this->getRegionFromStartVAddr(align.vaddrB);
  for (Addr vaddr = region.vaddr; vaddr < endVAddr; vaddr += pageSize) {
    auto pageVAddr = pTable->pageAlign(vaddr);
    auto pageHops =
        this->computeIndirectPageHops(tc, region, alignToRegion, pageVAddr);
    regionHops.pageHops.emplace_back(std::move(pageHops));
  }

  return regionHops;
}

StreamNUCAManager::IndirectPageHops StreamNUCAManager::computeIndirectPageHops(
    ThreadContext *tc, const StreamRegion &region,
    const StreamRegion &alignToRegion, Addr pageVAddr) {

  auto pTable = this->process->pTable;
  auto pageSize = pTable->getPageSize();
  auto totalSize = region.elementSize * region.numElement;
  auto endVAddr = std::min(region.vaddr + totalSize, pageVAddr + pageSize);
  auto numBytes = endVAddr - pageVAddr;
  auto pageIndex = (pageVAddr - region.vaddr) / pageSize;
  auto pagePAddr = this->translate(pageVAddr);
  auto defaultNodeId = StreamNUCAMap::mapPAddrToNUMAId(pagePAddr);

  const auto &memNodes = StreamNUCAMap::getNUMANodes();
  auto numMemNodes = memNodes.size();
  auto numRows = StreamNUCAMap::getNumRows();
  auto numCols = StreamNUCAMap::getNumCols();

  char *pageData = reinterpret_cast<char *>(malloc(pageSize));
  tc->getVirtProxy().readBlob(pageVAddr, pageData, pageSize);

  indRegionPages++;
  indRegionElements += numBytes / region.elementSize;

  IndirectPageHops pageHops(pageVAddr, pagePAddr, defaultNodeId, numMemNodes,
                            numRows * numCols);

  for (int i = 0; i < numBytes; i += region.elementSize) {
    int64_t index = 0;
    if (region.elementSize == 4) {
      index = *reinterpret_cast<int32_t *>(pageData + i);
    } else if (region.elementSize == 8) {
      index = *reinterpret_cast<int64_t *>(pageData + i);
    } else {
      panic("[StreamNUCA] Invalid IndrectRegion %s ElementSize %d.",
            region.name, region.elementSize);
    }
    if (index < 0 || index >= alignToRegion.numElement) {
      panic("[StreamNUCA] %s InvalidIndex %d not in %s NumElement %d.",
            region.name, index, alignToRegion.name, alignToRegion.numElement);
    }
    auto alignToVAddr = alignToRegion.vaddr + index * alignToRegion.elementSize;
    auto alignToPAddr = this->translate(alignToVAddr);
    auto alignToBank = StreamNUCAMap::getBank(alignToPAddr);

    // DPRINTF(StreamNUCAManager,
    //         "  Index %ld AlignToVAddr %#x AlignToPAddr %#x AlignToBank
    //         %d.\n", index, alignToVAddr, alignToPAddr, alignToBank);

    if (alignToBank < 0 || alignToBank >= pageHops.bankFreq.size()) {
      panic("[StreamNUCA] IndirectAlign %s -> %s Page %lu Index %ld Invalid "
            "AlignToBank %d.",
            region.name, alignToRegion.name, pageIndex, index, alignToBank);
    }
    pageHops.bankFreq.at(alignToBank)++;
    pageHops.totalElements++;

    // Accumulate the traffic hops for all NUMA nodes.
    for (int NUMAId = 0; NUMAId < numMemNodes; ++NUMAId) {
      const auto &memNode = memNodes.at(NUMAId);
      auto hops = StreamNUCAMap::computeHops(alignToBank, memNode.routerId);
      pageHops.hops.at(NUMAId) += hops;
    }
  }

  pageHops.maxHops = pageHops.hops.front();
  pageHops.minHops = pageHops.hops.front();
  pageHops.maxHopsNUMANodeId = 0;
  pageHops.minHopsNUMANodeId = 0;
  for (int NUMAId = 1; NUMAId < numMemNodes; ++NUMAId) {
    auto hops = pageHops.hops.at(NUMAId);
    if (hops > pageHops.maxHops) {
      pageHops.maxHops = hops;
      pageHops.maxHopsNUMANodeId = NUMAId;
    }
    if (hops < pageHops.minHops) {
      pageHops.minHops = hops;
      pageHops.minHopsNUMANodeId = NUMAId;
    }
  }

  return pageHops;
}

void StreamNUCAManager::greedyAssignIndirectPages(
    IndirectRegionHops &regionHops) {

  const auto numRows = StreamNUCAMap::getNumRows();
  const auto numCols = StreamNUCAMap::getNumCols();

  for (uint64_t pageIdx = 0; pageIdx < regionHops.pageHops.size(); ++pageIdx) {
    auto &pageHops = regionHops.pageHops.at(pageIdx);

    auto minHops = pageHops.minHops;
    auto minHopsNUMAId = pageHops.minHopsNUMANodeId;

    /**
     * Sort by their difference between MaxHops and MinHops.
     */
    regionHops.addRemapPageId(pageIdx, minHopsNUMAId);

    indRegionMemToLLCMinHops += minHops;
    indRegionMemMinBanks.sample(minHopsNUMAId, 1);

    if (Debug::StreamNUCAManager) {
      int32_t avgBankFreq = pageHops.totalElements / pageHops.bankFreq.size();
      std::stringstream freqMatrixStr;
      for (int row = 0; row < numRows; ++row) {
        for (int col = 0; col < numCols; ++col) {
          auto bank = row * numCols + col;
          freqMatrixStr << std::setw(6)
                        << (pageHops.bankFreq[bank] - avgBankFreq);
        }
        freqMatrixStr << '\n';
      }
      DPRINTF(StreamNUCAManager,
              "[StreamNUCA] IndRegion %s PageIdx %lu AvgBankFreq %d Diff:\n%s.",
              regionHops.region.name, pageIdx, avgBankFreq,
              freqMatrixStr.str());
    }
  }

  if (Debug::StreamNUCAManager) {
    const auto &region = regionHops.region;
    DPRINTF(StreamNUCAManager,
            "[StreamNUCA] IndirectRegion %s Finish Greedy Assign:\n",
            region.name);
    for (int i = 0; i < regionHops.numMemNodes; ++i) {
      auto pages = regionHops.remapPageIds.at(i).size();
      auto totalPages = regionHops.pageHops.size();
      auto ratio = static_cast<float>(pages) / static_cast<float>(totalPages);
      DPRINTF(StreamNUCAManager,
              "[StreamNUCA]     NUMANode %5d Pages %8lu %3.2f\n", i, pages,
              ratio * 100);
    }
  }
}

void StreamNUCAManager::IndirectRegionHops::addRemapPageId(uint64_t pageId,
                                                           int NUMANodeId) {
  /**
   * Sorted by their difference between MaxHops and MinHops.
   */
  auto &remapPageIds = this->remapPageIds.at(NUMANodeId);
  auto &remapPageHops = this->pageHops.at(pageId);
  remapPageHops.remapNUMANodeId = NUMANodeId;

  auto remapDiffHops = remapPageHops.maxHops - remapPageHops.minHops;
  auto iter = remapPageIds.begin();
  while (iter != remapPageIds.end()) {
    auto pageId = *iter;
    const auto &pageHops = this->pageHops.at(pageId);
    auto diffHops = pageHops.maxHops - pageHops.minHops;
    if (diffHops < remapDiffHops) {
      break;
    }
    ++iter;
  }
  remapPageIds.insert(iter, pageId);
}

void StreamNUCAManager::rebalanceIndirectPages(IndirectRegionHops &regionHops) {

  auto remapPageIdsCmp =
      [](const IndirectRegionHops::RemapPageIdsPerNUMANodeT &A,
         const IndirectRegionHops::RemapPageIdsPerNUMANodeT &B) -> bool {
    return A.size() < B.size();
  };

  auto isBalanced = [&regionHops, &remapPageIdsCmp]() -> bool {
    auto minMaxPair =
        std::minmax_element(regionHops.remapPageIds.begin(),
                            regionHops.remapPageIds.end(), remapPageIdsCmp);
    auto diff = minMaxPair.second->size() - minMaxPair.first->size();
    auto ratio = static_cast<float>(diff) /
                 static_cast<float>(regionHops.pageHops.size());
    const float threshold = 0.02f;
    return ratio <= threshold;
  };

  auto selectPopNUMAIter =
      [&regionHops,
       &remapPageIdsCmp]() -> IndirectRegionHops::RemapPageIdsT::iterator {
    // For now always select the NUMAId with most pages.
    auto maxIter =
        std::max_element(regionHops.remapPageIds.begin(),
                         regionHops.remapPageIds.end(), remapPageIdsCmp);
    return maxIter;
  };

  auto selectPushNUMAIter =
      [&regionHops,
       &remapPageIdsCmp]() -> IndirectRegionHops::RemapPageIdsT::iterator {
    // For now always select the NUMAId with least pages.
    auto minIter =
        std::min_element(regionHops.remapPageIds.begin(),
                         regionHops.remapPageIds.end(), remapPageIdsCmp);
    return minIter;
  };

  while (!isBalanced()) {
    auto popIter = selectPopNUMAIter();
    auto pushIter = selectPushNUMAIter();

    auto pageIdx = popIter->back();
    popIter->pop_back();

    auto pushNUMANodeId = pushIter - regionHops.remapPageIds.begin();
    regionHops.addRemapPageId(pageIdx, pushNUMANodeId);
  }

  if (Debug::StreamNUCAManager) {
    const auto &region = regionHops.region;
    DPRINTF(StreamNUCAManager,
            "[StreamNUCA] IndirectRegion %s Finish Rebalance:\n", region.name);
    for (int i = 0; i < regionHops.numMemNodes; ++i) {
      auto pages = regionHops.remapPageIds.at(i).size();
      auto totalPages = regionHops.pageHops.size();
      auto ratio = static_cast<float>(pages) / static_cast<float>(totalPages);
      DPRINTF(StreamNUCAManager,
              "[StreamNUCA]     NUMANode %5d Pages %8lu %3.2f\n", i, pages,
              ratio * 100);
    }
  }
}

void StreamNUCAManager::relocateIndirectPages(
    ThreadContext *tc, const IndirectRegionHops &regionHops) {

  auto pTable = this->process->pTable;
  auto pageSize = pTable->getPageSize();

  char *pageData = reinterpret_cast<char *>(malloc(pageSize));

  for (uint64_t pageIdx = 0; pageIdx < regionHops.pageHops.size(); ++pageIdx) {

    const auto &pageHops = regionHops.pageHops.at(pageIdx);
    auto remapNUMANodeId = pageHops.remapNUMANodeId;
    auto defaultNUMANodeId = pageHops.defaultNUMANodeId;

    if (!this->enableIndirectPageRemap) {
      /**
       * IndirectRemap is disabled, we just set remapNUMA = defaultNUMA.
       */
      remapNUMANodeId = defaultNUMANodeId;
    }

    this->indRegionMemToLLCDefaultHops += pageHops.hops.at(defaultNUMANodeId);
    this->indRegionMemToLLCRemappedHops += pageHops.hops.at(remapNUMANodeId);
    this->indRegionMemRemappedBanks.sample(remapNUMANodeId, 1);

    if (remapNUMANodeId == defaultNUMANodeId) {
      continue;
    }

    auto pageVAddr = pageHops.pageVAddr;
    auto defaultPagePAddr = pageHops.defaultPagePAddr;
    tc->getVirtProxy().readBlob(pageVAddr, pageData, pageSize);

    /**
     * Try to allocate a page at selected bank. Remap the vaddr to the new paddr
     * by setting clobber flag (which will destroy the old mapping). Then copy
     * the data.
     */
    int allocPages = 0;
    int allocNUMANodeId = 0;
    auto newPagePAddr = NUMAPageAllocator::allocatePageAt(
        tc->getProcessPtr()->system, remapNUMANodeId, allocPages,
        allocNUMANodeId);

    indRegionRemapPages++;
    indRegionAllocPages += allocPages;

    bool clobber = true;
    pTable->map(pageVAddr, newPagePAddr, pageSize, clobber);
    tc->getVirtProxy().writeBlob(pageVAddr, pageData, pageSize);

    // Return the old page to the allocator.
    NUMAPageAllocator::returnPage(defaultPagePAddr, defaultNUMANodeId);
  }
}

int64_t StreamNUCAManager::computeHopsAndFreq(
    const StreamRegion &region, const StreamRegion &alignToRegion,
    Addr pageVAddr, int64_t numBytes, char *pageData,
    std::vector<int32_t> &alignToBankFrequency) {

  auto pageSize = this->process->pTable->getPageSize();
  auto pageIndex = (pageVAddr - region.vaddr) / pageSize;

  int64_t traffic = 0;

  for (int i = 0; i < numBytes; i += region.elementSize) {
    int64_t index = 0;
    if (region.elementSize == 4) {
      index = *reinterpret_cast<int32_t *>(pageData + i);
    } else if (region.elementSize == 8) {
      index = *reinterpret_cast<int64_t *>(pageData + i);
    } else {
      panic("[StreamNUCA] Invalid IndrectRegion %s ElementSize %d.",
            region.name, region.elementSize);
    }
    if (index < 0 || index >= alignToRegion.numElement) {
      panic("[StreamNUCA] %s InvalidIndex %d not in %s NumElement %d.",
            region.name, index, alignToRegion.name, alignToRegion.numElement);
    }
    auto alignToVAddr = alignToRegion.vaddr + index * alignToRegion.elementSize;
    auto alignToPAddr = this->translate(alignToVAddr);
    auto alignToBank = StreamNUCAMap::getBank(alignToPAddr);

    // DPRINTF(StreamNUCAManager,
    //         "  Index %ld AlignToVAddr %#x AlignToPAddr %#x AlignToBank
    //         %d.\n", index, alignToVAddr, alignToPAddr, alignToBank);

    if (alignToBank < 0 || alignToBank >= alignToBankFrequency.size()) {
      panic("[StreamNUCA] IndirectAlign %s -> %s Page %lu Index %ld Invalid "
            "AlignToBank %d.",
            region.name, alignToRegion.name, pageIndex, index, alignToBank);
    }
    alignToBankFrequency.at(alignToBank)++;

    // Accumulate the default traffic hops.
    auto elementVAddr = pageVAddr + i;
    auto elementPAddr = this->translate(elementVAddr);
    auto elementBank = StreamNUCAMap::mapPAddrToNUMARouterId(elementPAddr);
    traffic += StreamNUCAMap::computeHops(alignToBank, elementBank);
  }

  return traffic;
}

void StreamNUCAManager::computeCacheSet() {

  /**
   * Compute the StartSet for arrays.
   * NOTE: We ignore indirect regions, as they will be remapped at page
   * granularity.
   * First group arrays by their alignment requirement.
   */
  std::map<Addr, std::vector<Addr>> alignRangeVAddrs;
  {
    std::map<Addr, Addr> unionFindParent;
    for (const auto &entry : this->startVAddrRegionMap) {
      unionFindParent.emplace(entry.first, entry.first);
    }

    auto find = [&unionFindParent](Addr vaddr) -> Addr {
      while (true) {
        auto iter = unionFindParent.find(vaddr);
        assert(iter != unionFindParent.end());
        if (iter->second == vaddr) {
          return vaddr;
        }
        vaddr = iter->second;
      }
    };

    auto merge = [&unionFindParent, &find](Addr vaddrA, Addr vaddrB) -> void {
      auto rootA = find(vaddrA);
      auto rootB = find(vaddrB);
      unionFindParent[rootA] = rootB;
    };

    for (const auto &entry : this->startVAddrRegionMap) {
      const auto &region = entry.second;
      for (const auto &align : region.aligns) {
        if (align.vaddrA == align.vaddrB) {
          // Ignore self alignment.
          continue;
        }
        merge(align.vaddrA, align.vaddrB);
        DPRINTF(StreamNUCAManager, "[AlignGroup] Union %#x %#x.\n",
                align.vaddrA, align.vaddrB);
      }
    }

    for (const auto &entry : unionFindParent) {
      /**
       * Ignore all indirect regions when contructing groups.
       */
      const auto &region = this->getRegionFromStartVAddr(entry.first);
      if (region.isIndirect) {
        continue;
      }
      auto root = find(entry.first);
      alignRangeVAddrs
          .emplace(std::piecewise_construct, std::forward_as_tuple(root),
                   std::forward_as_tuple())
          .first->second.emplace_back(entry.first);
    }
  }

  const auto totalBanks =
      StreamNUCAMap::getNumRows() * StreamNUCAMap::getNumCols();
  const auto llcNumSets = StreamNUCAMap::getCacheNumSet();
  const auto llcAssoc = StreamNUCAMap::getCacheAssoc();
  const auto llcBlockSize = StreamNUCAMap::getCacheBlockSize();
  const auto llcBankSize = llcNumSets * llcAssoc * llcBlockSize;
  const auto totalLLCSize = llcBankSize * totalBanks;

  for (auto &entry : alignRangeVAddrs) {
    auto &group = entry.second;
    // Sort for simplicity.
    std::sort(group.begin(), group.end());

    /**
     * First we estimate how many data can be cached.
     * NOTE: As a hack, we try the policy to avoid caching the out_neigh_index
     * if we can't fit all in the LLC.
     */
    auto totalElementSize = 0;
    auto totalSize = 0ul;
    for (auto startVAddr : group) {
      const auto &region = this->getRegionFromStartVAddr(startVAddr);
      totalElementSize += region.elementSize;
      totalSize += region.elementSize * region.numElement;
    }

    if (this->directRegionFitPolicy == DirectRegionFitPolicy::DROP &&
        totalSize > totalLLCSize) {
      for (auto iter = group.begin(), end = group.end(); iter != end; ++iter) {
        auto startVAddr = *iter;
        auto &region = this->getRegionFromStartVAddr(startVAddr);
        if (region.name == "gap.pr_push.out_neigh_index") {
          totalElementSize -= region.elementSize;
          totalSize -= region.elementSize * region.numElement;
          region.cachedElements = 0;
          group.erase(iter);
          // This is a vector, we have to break after erase something.
          DPRINTF(StreamNUCAManager,
                  "[AlignGroup] Avoid cache %s Bytes %lu ElementSize %lu.\n",
                  region.name, region.elementSize * region.numElement,
                  region.elementSize);
          break;
        }
      }
    }

    uint64_t cachedElements = totalLLCSize / totalElementSize;

    if (Debug::StreamNUCAManager) {
      DPRINTF(
          StreamNUCAManager,
          "[AlignGroup] Analyzing Group %#x NumRegions %d TotalElementSize %d "
          "CachedElements %lu.\n",
          group.front(), group.size(), totalElementSize, cachedElements);
      for (auto vaddr : group) {
        auto &region = this->getRegionFromStartVAddr(vaddr);
        DPRINTF(StreamNUCAManager,
                "[AlignGroup]   Region %#x Elements %lu Cached %.2f%%.\n",
                vaddr, region.numElement,
                static_cast<float>(cachedElements) /
                    static_cast<float>(region.numElement) * 100.f);
        region.cachedElements = std::min(cachedElements, region.numElement);
      }
    }

    auto startSet = 0;
    for (auto startVAddr : group) {
      const auto &region = this->getRegionFromStartVAddr(startVAddr);

      auto startPAddr = this->translate(startVAddr);
      auto &rangeMap = StreamNUCAMap::getRangeMapByStartPAddr(startPAddr);
      rangeMap.startSet = startSet;

      auto cachedBytes = cachedElements * region.elementSize;
      auto usedSets = cachedBytes / (llcBlockSize * totalBanks);

      DPRINTF(
          StreamNUCAManager,
          "Range %s %#x ElementSize %d CachedElements %lu StartSet %d UsedSet "
          "%d.\n",
          region.name, region.vaddr, region.elementSize, cachedElements,
          startSet, usedSets);
      startSet = (startSet + usedSets) % llcNumSets;
    }
  }
}

StreamNUCAManager::StreamRegion &
StreamNUCAManager::getRegionFromStartVAddr(Addr vaddr) {
  auto iter = this->startVAddrRegionMap.find(vaddr);
  if (iter == this->startVAddrRegionMap.end()) {
    panic("Failed to find StreamRegion at %#x.", vaddr);
  }
  return iter->second;
}

StreamNUCAManager::StreamRegion &
StreamNUCAManager::getRegionFromName(const std::string &name) {
  for (auto &entry : this->startVAddrRegionMap) {
    auto &region = entry.second;
    if (region.name == name) {
      return region;
    }
  }
  panic("Failed to find StreamRegion %s.", name);
}

bool StreamNUCAManager::isPAddrContinuous(const StreamRegion &region) {
  auto pTable = this->process->pTable;
  auto pageSize = pTable->getPageSize();
  auto startPageVAddr = pTable->pageAlign(region.vaddr);
  Addr startPagePAddr;
  if (!pTable->translate(startPageVAddr, startPagePAddr)) {
    panic("StreamNUCAManager failed to translate StartVAddr %#x.",
          region.vaddr);
  }
  auto endVAddr = region.vaddr + region.elementSize * region.numElement;
  for (auto vaddr = startPageVAddr; vaddr < endVAddr; vaddr += pageSize) {
    Addr paddr;
    if (!pTable->translate(vaddr, paddr)) {
      panic("StreamNUCAManager failed to translate vaddr %#x, StartVAddr %#x.",
            vaddr, startPageVAddr);
    }
    if (paddr - startPagePAddr != vaddr - startPageVAddr) {
      DPRINTF(
          StreamNUCAManager,
          "Range %s StartVAddr %#x StartPageVAddr %#x StartPagePAddr %#x not "
          "physically continuous at %#x paddr %#x.\n",
          region.name, region.vaddr, startPageVAddr, startPagePAddr, vaddr,
          paddr);
      return false;
    }
  }
  return true;
}

Addr StreamNUCAManager::translate(Addr vaddr) {
  Addr paddr;
  if (!this->process->pTable->translate(vaddr, paddr)) {
    panic("[StreamNUCA] failed to translate VAddr %#x.", vaddr);
  }
  return paddr;
}

uint64_t StreamNUCAManager::determineInterleave(const StreamRegion &region) {
  const uint64_t defaultInterleave = 1024;
  uint64_t interleave = defaultInterleave;

  auto numRows = StreamNUCAMap::getNumRows();
  auto numCols = StreamNUCAMap::getNumCols();
  auto numBanks = numRows * numCols;

  auto defaultWrapAroundBytes = defaultInterleave * numBanks;
  auto defaultColWrapAroundBytes = defaultInterleave * numCols;

  for (const auto &align : region.aligns) {
    const auto &alignToRegion = this->getRegionFromStartVAddr(align.vaddrB);

    auto elementOffset = align.elementOffset;
    auto bytesOffset = elementOffset * alignToRegion.elementSize;
    DPRINTF(StreamNUCAManager,
            "Range %s %#x AlignTo %#x Offset Element %ld Bytes %lu.\n",
            region.name, region.vaddr, alignToRegion.vaddr, elementOffset,
            bytesOffset);

    if (elementOffset < 0) {
      panic("Range %s %#x with negative element offset %ld.\n", region.name,
            region.vaddr, elementOffset);
    }

    if ((&alignToRegion) == (&region)) {
      // Self alignment.
      if ((bytesOffset % defaultWrapAroundBytes) == 0) {
        // Already aligned.
        DPRINTF(StreamNUCAManager, "Range %s %#x Self Aligned.\n", region.name,
                region.vaddr);
      } else if ((bytesOffset % defaultColWrapAroundBytes) == 0) {
        // Try to align with one row.
        interleave =
            bytesOffset / defaultColWrapAroundBytes * defaultInterleave;
        DPRINTF(StreamNUCAManager,
                "Range %s %#x Self Aligned To Row Interleave %lu = %lu / %lu * "
                "%lu.\n",
                region.name, region.vaddr, interleave, bytesOffset,
                defaultColWrapAroundBytes, defaultInterleave);
      } else {
        panic("Not Support Yet: Range %s %#x Self Align ElementOffset %lu "
              "ByteOffset %lu.\n",
              region.name, region.vaddr, align.elementOffset, bytesOffset);
      }
    } else {
      // Other alignment.
      auto otherInterleave = this->determineInterleave(alignToRegion);
      DPRINTF(StreamNUCAManager,
              "Range %s %#x Align to Range %#x Interleave = %lu / %lu * %lu.\n",
              region.name, region.vaddr, alignToRegion.vaddr, otherInterleave,
              alignToRegion.elementSize, region.elementSize);
      interleave =
          otherInterleave / alignToRegion.elementSize * region.elementSize;
    }
  }
  return interleave;
}

int StreamNUCAManager::determineStartBank(const StreamRegion &region,
                                          uint64_t interleave) {

  auto startVAddr = region.vaddr;
  auto startPAddr = this->translate(startVAddr);

  int startBank = 0;
  if (region.name.find("rodinia.pathfinder") == 0 ||
      region.name.find("rodinia.hotspot3D") == 0 ||
      region.name.find("gap.pr_push.") == 0) {
    // Pathfinder need to start at the original bank.
    startBank = (startPAddr / interleave) %
                (StreamNUCAMap::getNumCols() * StreamNUCAMap::getNumRows());
  }

  for (const auto &align : region.aligns) {
    if (align.vaddrB == align.vaddrA) {
      continue;
    }
    /**
     * Use alignToRegion startBank.
     */
    const auto &alignToRegion = this->getRegionFromStartVAddr(align.vaddrB);
    auto alignToRegionStartPAddr = this->translate(align.vaddrB);
    const auto &alignToRegionMap =
        StreamNUCAMap::getRangeMapByStartPAddr(alignToRegionStartPAddr);
    startBank = alignToRegionMap.startBank;
    DPRINTF(StreamNUCAManager,
            "[StreamNUCA] Region %s Align StartBank %d to %s.\n", region.name,
            startBank, alignToRegion.name);
  }

  return startBank;
}

uint64_t StreamNUCAManager::getCachedBytes(Addr start) {
  const auto &region = this->getRegionFromStartVAddr(start);
  return region.cachedElements * region.elementSize;
}