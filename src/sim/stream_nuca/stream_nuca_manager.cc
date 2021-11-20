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
Stats::ScalarNoReset StreamNUCAManager::indRegionMemToLLCDefaultHops;
Stats::ScalarNoReset StreamNUCAManager::indRegionMemToLLCOptimizedHops;
Stats::DistributionNoReset StreamNUCAManager::indRegionMemOptimizedBanks;
Stats::DistributionNoReset StreamNUCAManager::indRegionMemRemappedBanks;

StreamNUCAManager::StreamNUCAManager(const StreamNUCAManager &other)
    : process(other.process), enabled(other.enabled) {
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
  scalar(indRegionMemToLLCDefaultHops,
         "Default hops from Mem to LLC in indirect region.");
  scalar(indRegionMemToLLCOptimizedHops,
         "Optimized hops from Mem to LLC in indirect retion.");

  auto numMemNodes = StreamNUCAMap::getNUMANodes().size();
  distribution(indRegionMemOptimizedBanks, 0, numMemNodes, 1,
               "Distribution of optimized IndRegion banks.");
  distribution(indRegionMemRemappedBanks, 0, numMemNodes, 1,
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
          "AllocPages %lu DefaultHops %lu OptHops %lu.\n",
          static_cast<uint64_t>(indRegionPages.value()),
          static_cast<uint64_t>(indRegionElements.value()),
          static_cast<uint64_t>(indRegionAllocPages.value()),
          static_cast<uint64_t>(indRegionMemToLLCDefaultHops.value()),
          static_cast<uint64_t>(indRegionMemToLLCOptimizedHops.value()));
}

void StreamNUCAManager::remapRegion(ThreadContext *tc,
                                    const StreamRegion &region) {
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
    panic("Region %s %#x PAddr is not continuous.", region.name, region.vaddr);
  }
  auto startVAddr = region.vaddr;
  auto startPAddr = this->translate(startVAddr);

  auto endPAddr = startPAddr + region.elementSize * region.numElement;

  uint64_t interleave = this->determineInterleave(region);
  int startBank = 0;
  int startSet = 0;

  if (region.name.find("rodinia.pathfinder") == 0 ||
      region.name.find("rodinia.hotspot3D") == 0) {
    // Pathfinder need to start at the original bank.
    startBank = (startPAddr / interleave) %
                (StreamNUCAMap::getNumCols() * StreamNUCAMap::getNumRows());
  }

  StreamNUCAMap::addRangeMap(startPAddr, endPAddr, interleave, startBank,
                             startSet);
  DPRINTF(StreamNUCAManager,
          "Map Region %s %#x PAddr %#x Interleave %lu Bank %d.\n", region.name,
          startVAddr, startPAddr, interleave, startBank);
}

void StreamNUCAManager::remapIndirectRegion(ThreadContext *tc,
                                            const StreamRegion &region) {
  assert(region.aligns.size() == 1 &&
         "IndirectRegion should have only one align.");
  const auto &align = region.aligns.front();
  assert(align.vaddrB != region.vaddr && "Self-IndirectAlign?");

  /**
   * Scan through the indirect regions and collect outgoing banks.
   * Then remap each page to reduce traffic.
   */
  auto pTable = this->process->pTable;
  auto pageSize = pTable->getPageSize();
  auto totalSize = region.elementSize * region.numElement;
  auto endVAddr = region.vaddr + totalSize;
  if (pTable->pageOffset(region.vaddr) != 0) {
    panic("[StreamNUCA] IndirectRegion %s VAddr %#x should align to page.",
          region.name, region.vaddr);
  }
  const auto &alignToRegion = this->getRegionFromStartVAddr(align.vaddrB);
  for (Addr vaddr = region.vaddr; vaddr < endVAddr; vaddr += pageSize) {
    auto pageVAddr = pTable->pageAlign(vaddr);
    this->remapIndirectPage(tc, region, alignToRegion, pageVAddr);
  }
}

void StreamNUCAManager::remapIndirectPage(ThreadContext *tc,
                                          const StreamRegion &region,
                                          const StreamRegion &alignToRegion,
                                          Addr pageVAddr) {
  auto pTable = this->process->pTable;
  auto pageSize = pTable->getPageSize();
  auto totalSize = region.elementSize * region.numElement;
  auto endVAddr = std::min(region.vaddr + totalSize, pageVAddr + pageSize);
  auto numBytes = endVAddr - pageVAddr;
  auto pageIndex = (pageVAddr - region.vaddr) / pageSize;

  char *pageData = reinterpret_cast<char *>(malloc(pageSize));
  tc->getVirtProxy().readBlob(pageVAddr, pageData, pageSize);

  indRegionPages++;
  indRegionElements += numBytes / region.elementSize;

  /**
   * Read each element and count the frequency of AlignToBank.
   */
  auto numRows = StreamNUCAMap::getNumRows();
  auto numCols = StreamNUCAMap::getNumCols();
  std::vector<int32_t> alignToBankFrequency(numRows * numCols, 0);

  DPRINTF(StreamNUCAManager,
          "[StreamNUCA] IndirectAlign %s -> %s PageIndex %lu.\n", region.name,
          alignToRegion.name, pageIndex);

  auto defaultHops =
      this->computeHopsAndFreq(region, alignToRegion, pageVAddr, numBytes,
                               pageData, alignToBankFrequency);
  indRegionMemToLLCDefaultHops += defaultHops;

  /**
   * For all valid NUMA nodes, select the one has lowest traffic.
   */
  const auto &numaNodes = StreamNUCAMap::getNUMANodes();
  int selectedNUMANode = -1;
  int selectedBank = -1;
  int64_t selectedNUMANodeTraffic = 0;
  for (int i = 0; i < numaNodes.size(); ++i) {
    const auto &numaNode = numaNodes.at(i);
    int64_t traffic = 0;
    for (int bank = 0; bank < alignToBankFrequency.size(); ++bank) {
      traffic += StreamNUCAMap::computeHops(bank, numaNode.routerId) *
                 alignToBankFrequency.at(bank);
    }
    DPRINTF(StreamNUCAManager, "  NUMA %d Router %d Traffic %ld.\n", i,
            numaNode.routerId, traffic);
    if (selectedNUMANode == -1 || traffic < selectedNUMANodeTraffic) {
      selectedNUMANode = i;
      selectedBank = numaNode.routerId;
      selectedNUMANodeTraffic = traffic;
    }
  }

  if (Debug::StreamNUCAManager) {
    int32_t avgBankFreq =
        (numBytes / region.elementSize) / alignToBankFrequency.size();
    std::stringstream freqMatrixStr;
    for (int row = 0; row < numRows; ++row) {
      for (int col = 0; col < numCols; ++col) {
        auto bank = row * numCols + col;
        freqMatrixStr << std::setw(6)
                      << (alignToBankFrequency[bank] - avgBankFreq);
      }
      freqMatrixStr << '\n';
    }
    DPRINTF(StreamNUCAManager, "[StreamNUCA] AvgBankFreq %d Diff:\n%s.",
            avgBankFreq, freqMatrixStr.str());
  }
  indRegionMemOptimizedBanks.sample(selectedNUMANode, 1);

  /**
   * Try to allocate a page at selected bank. Remap the vaddr to the new paddr
   * by setting clobber flag (which will destroy the old mapping). Then copy the
   * data.
   */
  int allocPages = 0;
  int allocNodeId = 0;
  auto newPagePAddr = NUMAPageAllocator::allocatePageAt(
      process->system, selectedNUMANode, allocPages, allocNodeId);
  indRegionAllocPages += allocPages;
  indRegionMemRemappedBanks.sample(allocNodeId, 1);

  bool clobber = true;
  pTable->map(pageVAddr, newPagePAddr, pageSize, clobber);
  tc->getVirtProxy().writeBlob(pageVAddr, pageData, pageSize);

  // Compute the optimized hops. The frequence should not change.
  std::vector<int32_t> optimizedAlignToBankFrequency(numRows * numCols, 0);
  auto optimizedHops =
      this->computeHopsAndFreq(region, alignToRegion, pageVAddr, numBytes,
                               pageData, optimizedAlignToBankFrequency);
  indRegionMemToLLCOptimizedHops += optimizedHops;

  DPRINTF(StreamNUCAManager,
          "[StreamNUCA] IndirectAlign %s -> %s PageIndex %lu SelectedBank "
          "%d SelectedNUMA %d DefaultHops %ld OptimizedHops %ld.\n",
          region.name, alignToRegion.name, pageIndex, selectedBank,
          selectedNUMANode, defaultHops, optimizedHops);

  free(pageData);
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
     */
    auto totalElementSize = 0;
    for (auto startVAddr : group) {
      const auto &region = this->getRegionFromStartVAddr(startVAddr);
      totalElementSize += region.elementSize;
    }
    auto cachedElements = totalLLCSize / totalElementSize;

    if (Debug::StreamNUCAManager) {
      DPRINTF(
          StreamNUCAManager,
          "[AlignGroup] Analyzing Group %#x NumRegions %d TotalElementSize %d "
          "CachedElements %d.\n",
          group.front(), group.size(), totalElementSize, cachedElements);
      for (auto vaddr : group) {
        DPRINTF(StreamNUCAManager, "[AlignGroup]   Region %#x.\n", vaddr);
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
          "Range %s %#x ElementSize %d CachedElements %d StartSet %d UsedSet "
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
      DPRINTF(StreamNUCAManager,
              "Range %s %#x not physically continuous at %#x paddr %#x.\n",
              region.name, region.vaddr, vaddr, paddr);
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