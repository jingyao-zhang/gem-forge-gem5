#include "stream_nuca_manager.hh"
#include "stream_nuca_map.hh"

#include "base/trace.hh"

#include <unordered_set>

#include "debug/StreamNUCAManager.hh"

StreamNUCAManager::StreamNUCAManager(const StreamNUCAManager &other) {
  panic("StreamNUCAManager does not have copy constructor.");
}

StreamNUCAManager &
StreamNUCAManager::operator=(const StreamNUCAManager &other) {
  panic("StreamNUCAManager does not have copy constructor.");
}

void StreamNUCAManager::defineRegion(const std::string &regionName, Addr start,
                                     uint64_t elementSize,
                                     uint64_t numElement) {
  DPRINTF(StreamNUCAManager, "Define Region %s %#x %lu %lu.\n", regionName,
          start, elementSize, numElement);
  this->startVAddrRegionMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(start),
      std::forward_as_tuple(regionName, start, elementSize, numElement));
}

void StreamNUCAManager::defineAlign(Addr A, Addr B, uint64_t elementOffset) {
  DPRINTF(StreamNUCAManager, "Define Align %#x %#x Offset %lu.\n", A, B,
          elementOffset);
  auto &regionA = this->getRegionFromStartVAddr(A);
  regionA.aligns.emplace_back(A, B, elementOffset);
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

void StreamNUCAManager::remap() {
  DPRINTF(StreamNUCAManager, "Remap Regions Enabled %d.\n", this->enabled);
  if (!this->enabled) {
    return;
  }

  for (const auto &entry : this->startVAddrRegionMap) {
    const auto &region = entry.second;
    if (!this->isPAddrContinuous(region)) {
      panic("Region %s %#x PAddr is not continuous.", region.name,
            region.vaddr);
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
            "Map Region %s %#x PAddr %#x Interleave %lu Bank %d.\n",
            region.name, startVAddr, startPAddr, interleave, startBank);
  }

  this->computeCacheSet();
}

void StreamNUCAManager::computeCacheSet() {

  /**
   * Compute the StartSet for arrays.
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
    panic("StreamNUCAManager failed to translate VAddr %#x.", vaddr);
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
            "Range %s %#x AlignTo %#x Offset Element %lu Bytes %lu.\n",
            region.name, region.vaddr, alignToRegion.vaddr, elementOffset,
            bytesOffset);

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