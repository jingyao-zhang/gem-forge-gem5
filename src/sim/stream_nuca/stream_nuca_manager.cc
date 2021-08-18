#include "stream_nuca_manager.hh"
#include "stream_nuca_map.hh"

#include "base/trace.hh"

#include "debug/StreamNUCAManager.hh"

StreamNUCAManager::StreamNUCAManager(const StreamNUCAManager &other) {
  panic("StreamNUCAManager does not have copy constructor.");
}

StreamNUCAManager &
StreamNUCAManager::operator=(const StreamNUCAManager &other) {
  panic("StreamNUCAManager does not have copy constructor.");
}

void StreamNUCAManager::defineRegion(Addr start, uint64_t elementSize,
                                     uint64_t numElement) {
  DPRINTF(StreamNUCAManager, "Define Region %#x %lu %lu.\n", start, elementSize,
          numElement);
  this->startVAddrRegionMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(start),
      std::forward_as_tuple(start, elementSize, numElement));
}

void StreamNUCAManager::defineAlign(Addr A, Addr B, uint64_t elementOffset) {
  DPRINTF(StreamNUCAManager, "Define Align %#x %#x Offlset %lu.\n", A, B,
          elementOffset);
  auto &regionA = this->getRegionFromStartVAddr(A);
  regionA.aligns.emplace_back(A, B, elementOffset);
}

void StreamNUCAManager::remap() {
  DPRINTF(StreamNUCAManager, "Remap Regions Enabled %d.\n", this->enabled);
  if (!this->enabled) {
    return;
  }

  /**
   * For now we just interleave all array with 1kB.
   */
  for (const auto &entry : this->startVAddrRegionMap) {
    const auto &region = entry.second;
    if (!this->isPAddrContinuous(region)) {
      panic("Region %#x PAddr is not continuous.", region.vaddr);
    }
    auto startVAddr = region.vaddr;
    auto startPAddr = this->translate(startVAddr);

    auto endPAddr = startPAddr + region.elementSize * region.numElement;

    uint64_t interleave = 1024;
    int startBank = 0;
    StreamNUCAMap::addRangeMap(startPAddr, endPAddr, interleave, startBank);
    DPRINTF(StreamNUCAManager,
            "Map Region %#x PAddr %#x Interleave %lu Bank %d.\n", startVAddr,
            startPAddr, interleave, startBank);
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
              "Range %#x not physically continuous at %#x paddr %#x.\n",
              region.vaddr, vaddr, paddr);
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