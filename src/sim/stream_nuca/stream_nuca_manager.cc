#include "stream_nuca_manager.hh"
#include "numa_page_allocator.hh"
#include "stream_nuca_map.hh"

#include "base/trace.hh"
#include "cpu/gem_forge/accelerator/stream/cache/pum/PUMHWConfiguration.hh"
#include "cpu/thread_context.hh"
#include "params/Process.hh"

#include <iomanip>
#include <unordered_set>

#include "debug/StreamNUCAManager.hh"

std::shared_ptr<StreamNUCAManager> StreamNUCAManager::singleton = nullptr;

// There is only one StreamNUCAManager.
std::shared_ptr<StreamNUCAManager>
StreamNUCAManager::initialize(Process *_process, ProcessParams *_params) {
  if (!singleton) {
    singleton = std::make_shared<StreamNUCAManager>(_process, _params);
  }
  return singleton;
}

StreamNUCAManager::StreamNUCAManager(Process *_process, ProcessParams *_params)
    : process(_process), enabledMemStream(_params->enableMemStream),
      enabledNUCA(_params->enableStreamNUCA),
      enablePUM(_params->enableStreamPUMMapping),
      enablePUMTiling(_params->enableStreamPUMTiling),
      forcePUMTilingDim(_params->forceStreamPUMTilingDim),
      forcePUMTilingSize(_params->forceStreamPUMTilingSize),
      indirectRemapBoxBytes(_params->streamNUCAIndRemapBoxBytes),
      indirectRebalanceThreshold(_params->streamNUCAIndRebalanceThreshold) {
  const auto &directRegionFitPolicy = _params->streamNUCADirectRegionFitPolicy;
  if (directRegionFitPolicy == "crop") {
    this->directRegionFitPolicy = DirectRegionFitPolicy::CROP;
  } else if (directRegionFitPolicy == "drop") {
    this->directRegionFitPolicy = DirectRegionFitPolicy::DROP;
  } else {
    panic("Unknown DirectRegionFitPolicy %s.", directRegionFitPolicy);
  }
}

StreamNUCAManager::StreamNUCAManager(const StreamNUCAManager &other)
    : process(other.process), enabledMemStream(other.enabledMemStream),
      enabledNUCA(other.enabledNUCA), enablePUM(other.enablePUM),
      enablePUMTiling(other.enablePUMTiling),
      forcePUMTilingSize(other.forcePUMTilingSize),
      directRegionFitPolicy(other.directRegionFitPolicy),
      indirectRemapBoxBytes(other.indirectRemapBoxBytes) {
  panic("StreamNUCAManager does not have copy constructor.");
}

StreamNUCAManager &
StreamNUCAManager::operator=(const StreamNUCAManager &other) {
  panic("StreamNUCAManager does not have copy constructor.");
}

void StreamNUCAManager::regStats() {

  if (this->statsRegisterd) {
    return;
  }
  this->statsRegisterd = true;

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

  scalar(indRegionBoxes, "Pages in indirect region.");
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

  auto numBanks = StreamNUCAMap::getNumRows() * StreamNUCAMap::getNumCols();
  distribution(indRegionMemMinBanks, 0, numBanks - 1, 1,
               "Distribution of minimal IndRegion banks.");
  distribution(indRegionMemRemappedBanks, 0, numBanks - 1, 1,
               "Distribution of remapped IndRegion banks.");

#undef distribution
#undef scalar
}

void StreamNUCAManager::defineRegion(const std::string &regionName, Addr start,
                                     uint64_t elementSize,
                                     const std::vector<int64_t> &arraySizes) {

  int64_t numElement = 1;
  for (const auto &s : arraySizes) {
    numElement *= s;
  }
  DPRINTF(StreamNUCAManager,
          "[StreamNUCA] Define Region %s %#x %ld %ld=%ldx%ldx%ld %lukB.\n",
          regionName, start, elementSize, numElement, arraySizes[0],
          arraySizes.size() > 1 ? arraySizes[1] : 1,
          arraySizes.size() > 2 ? arraySizes[2] : 1,
          elementSize * numElement / 1024);
  this->startVAddrRegionMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(start),
      std::forward_as_tuple(regionName, start, elementSize, numElement,
                            arraySizes));
}

void StreamNUCAManager::setProperty(Addr start, uint64_t property,
                                    uint64_t value) {
  DPRINTF(StreamNUCAManager, "[StreamNUCA] Set Property %#x %lu Value %lu.\n",
          start, property, value);
  auto &region = this->getRegionFromStartVAddr(start);
  switch (property) {
  default: {
    panic("[StreamNUCA] Invalid property %lu.", property);
  }
#define CASE(E)                                                                \
  case RegionProperty::E: {                                                    \
    region.userDefinedProperties.emplace(RegionProperty::E, value);            \
    break;                                                                     \
  }

    CASE(INTERLEAVE);
    CASE(USE_PUM);
    CASE(PUM_NO_INIT);
    CASE(PUM_TILE_SIZE_DIM0);
    CASE(REDUCE_DIM);
    CASE(BROADCAST_DIM);

#undef CASE
  }
}

void StreamNUCAManager::defineAlign(Addr A, Addr B, int64_t elemOffset) {
  DPRINTF(StreamNUCAManager, "[StreamNUCA] Define Align %#x %#x Offset %ld.\n",
          A, B, elemOffset);
  auto &regionA = this->getRegionFromStartVAddr(A);
  regionA.aligns.emplace_back(A, B, elemOffset);
  if (elemOffset < 0) {
    regionA.isIrregular = true;
    IrregularAlignField indField = decodeIrregularAlign(elemOffset);
    if (indField.type == IrregularAlignField::TypeE::PtrChase) {
      regionA.isPtrChase = true;
    }
    DPRINTF(StreamNUCAManager,
            "[StreamNUCA]     IrregularAlign Type %d Offset %d Size %d.\n",
            indField.type, indField.offset, indField.size);
  }
}

const StreamNUCAManager::StreamRegion *
StreamNUCAManager::tryGetContainingStreamRegion(Addr vaddr) const {
  auto iter = this->startVAddrRegionMap.upper_bound(vaddr);
  if (iter == this->startVAddrRegionMap.begin()) {
    return nullptr;
  }
  iter--;
  const auto &region = iter->second;
  if (region.vaddr + region.elementSize * region.numElement <= vaddr) {
    return nullptr;
  }
  return &region;
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
  DPRINTF(StreamNUCAManager,
          "Remap Regions EnabledMemStream %d EnabledNUCA %d.\n",
          this->enabledMemStream, this->enabledNUCA);

  /**
   * Even not enabled, we group direct regions by their alignement.
   * Also, if we enabled memory stream, we try to compute cached elements.
   */
  this->groupDirectRegionsByAlign();

  if (this->enabledMemStream) {
    this->computeCachedElements();
  }

  if (!this->enabledNUCA) {
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
  std::vector<Addr> sortedRegionVAddrs;
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
        sortedRegionVAddrs.push_back(regionVAddr);
        regionRemapStateMap.at(regionVAddr) = 2;
        stack.pop_back();

      } else {
        // This region is already remapped. Ignore it.
        stack.pop_back();
      }
    }
  }

  this->remapRegions(tc, sortedRegionVAddrs);

  this->computeCacheSet();

  DPRINTF(StreamNUCAManager,
          "[StreamNUCA] Remap Done. IndRegion: Boxes %lu Elems %lu "
          "AllocBoxes %lu RemapBoxes %lu DefaultHops %lu MinHops %lu RemapHops "
          "%lu.\n",
          static_cast<uint64_t>(indRegionBoxes.value()),
          static_cast<uint64_t>(indRegionElements.value()),
          static_cast<uint64_t>(indRegionAllocPages.value()),
          static_cast<uint64_t>(indRegionRemapPages.value()),
          static_cast<uint64_t>(indRegionMemToLLCDefaultHops.value()),
          static_cast<uint64_t>(indRegionMemToLLCMinHops.value()),
          static_cast<uint64_t>(indRegionMemToLLCRemappedHops.value()));
}

void StreamNUCAManager::remapRegions(ThreadContext *tc,
                                     const AddrVecT &regionVAddrs) {

  /**
   * Collect remap decision for each region.
   */
  std::vector<int> remapDecisions;
  std::vector<Addr> remapPUMRegionVAddrs;
  const int REMAP_INDIRECT = 0;
  const int REMAP_NUCA = 1;
  const int REMAP_PUM = 2;
  const int REMAP_PTR_CHASE = 3;
  for (const auto &regionVAddr : regionVAddrs) {

    auto &region = this->getRegionFromStartVAddr(regionVAddr);

    if (region.isIrregular) {
      if (region.isPtrChase) {
        remapDecisions.push_back(REMAP_PTR_CHASE);
      } else {
        remapDecisions.push_back(REMAP_INDIRECT);
      }
    } else {
      if (this->enablePUM && this->canRemapDirectRegionPUM(region)) {
        remapDecisions.push_back(REMAP_PUM);
        remapPUMRegionVAddrs.push_back(regionVAddr);
      } else {
        remapDecisions.push_back(REMAP_NUCA);
      }
    }
  }

  /**
   * For now we enforce the same virtual bitlines for all PUM region.
   */
  int64_t vBitlines = this->getVirtualBitlinesForPUM(remapPUMRegionVAddrs);
  for (int i = 0; i < regionVAddrs.size(); ++i) {
    auto decision = remapDecisions.at(i);
    auto &region = this->getRegionFromStartVAddr(regionVAddrs.at(i));
    switch (decision) {
    default:
      panic("Invalid Remap Decision %d.", decision);
    case REMAP_INDIRECT: {
      this->remapIndirectRegion(tc, region);
      break;
    }
    case REMAP_PTR_CHASE: {
      this->remapPtrChaseRegion(tc, region);
      break;
    }
    case REMAP_NUCA: {
      this->remapDirectRegionNUCA(region);
      break;
    }
    case REMAP_PUM: {
      this->remapDirectRegionPUM(region, vBitlines);
      break;
    }
    }
  }
}

void StreamNUCAManager::remapDirectRegionNUCA(const StreamRegion &region) {
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

void StreamNUCAManager::remapPtrChaseRegion(ThreadContext *tc,
                                            StreamRegion &region) {

  /**
   * The aligned to array should be the head array.
   */
  assert(region.aligns.size() == 1);
  const auto &align = region.aligns.front();
  auto alignToRegionVAddr = align.vaddrB;
  const auto &alignToRegion = this->getRegionFromStartVAddr(alignToRegionVAddr);

  auto alignInfo = decodeIrregularAlign(align.elemOffset);
  assert(alignInfo.type == IrregularAlignField::TypeE::PtrChase);

  DPRINTF(
      StreamNUCAManager,
      "[StreamNUCA] Remap PtrCahse %s Head %s Offset %d Size %d ElemSize %d.\n",
      region.name, alignToRegion.name, alignInfo.offset, alignInfo.size,
      region.elementSize);

  const auto llcBlockSize = StreamNUCAMap::getCacheBlockSize();
  const auto nodeSize = region.elementSize;

  assert(nodeSize % llcBlockSize == 0);
  assert(nodeSize >= llcBlockSize);
  assert(alignToRegion.elementSize == 8);

  std::unordered_map<Addr, int> paddrLineToBankMap;
  const auto numLists = alignToRegion.numElement;

  auto &virtProxy = tc->getVirtProxy();
  auto totalNodes = 0;
  auto totalBanks = StreamNUCAMap::getNumRows() * StreamNUCAMap::getNumCols();
  auto currentBank = 0;
  for (int i = 0; i < numLists; ++i) {
    auto listVAddr = alignToRegion.vaddr + i * alignToRegion.elementSize;
    Addr headVAddr;
    virtProxy.readBlob(listVAddr, &headVAddr, alignToRegion.elementSize);
    assert(headVAddr != 0);

    Addr headPAddr;

    int count = 0;

    while (headVAddr != 0) {

      // Assume one node will not span across multiple pages.
      assert(tc->getProcessPtr()->pTable->translate(headVAddr, headPAddr));
      for (Addr paddr = headPAddr; paddr < headPAddr + nodeSize; ++paddr) {
        paddrLineToBankMap.emplace(paddr, currentBank);
      }

      Addr nextPtr = headVAddr + alignInfo.offset;
      Addr nextVAddr;
      virtProxy.readBlob(nextPtr, &nextVAddr, alignInfo.size);

      headVAddr = nextVAddr;

      count++;
    }

    totalNodes += count;

    // Round robin assign to bank.
    currentBank = (currentBank + 1) % totalBanks;
  }

  DPRINTF(StreamNUCAManager, "[StreamNUCA] Remapped %d PtrChase Nodes.\n",
          totalNodes);

  Addr startPAddr;
  assert(tc->getProcessPtr()->pTable->translate(region.vaddr, startPAddr));

  // For now add the default mapping.
  StreamNUCAMap::addRangeMap(startPAddr, startPAddr + region.numElement *
                                                          region.elementSize);
  StreamNUCAMap::overridePAddrToBank(paddrLineToBankMap);
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
   * NOTE: This does not work with PUM.
   * NOTE: For now remapped indirect region is not cached.
   */
  if (this->enablePUM) {
    panic("[StreamNUCA] IndirectRegion with PUM.");
  }

  // Register a default region.
  Addr startPAddr;
  assert(tc->getProcessPtr()->pTable->translate(region.vaddr, startPAddr));
  StreamNUCAMap::addRangeMap(startPAddr, startPAddr + region.numElement *
                                                          region.elementSize);

  region.cachedElements = region.numElement;
  if (this->indirectRemapBoxBytes == 0) {
    // Indirect remap is disabled.
    return;
  }

  auto regionHops = this->computeIndirectRegionHops(tc, region);

  this->greedyAssignIndirectBoxes(regionHops);
  if (this->indirectRebalanceThreshold > 0.0f) {
    this->rebalanceIndirectBoxes(regionHops);
  }

  // Register a fake region.
  this->relocateIndirectBoxes(tc, regionHops);
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
  auto boxSize = this->indirectRemapBoxBytes;
  auto totalSize = region.elementSize * region.numElement;
  auto endVAddr = region.vaddr + totalSize;
  if (pTable->pageOffset(region.vaddr) != 0) {
    panic("[StreamNUCA] IndirectRegion %s VAddr %#x should align to page.",
          region.name, region.vaddr);
  }

  IndirectRegionHops regionHops(region, StreamNUCAMap::getNumCols() *
                                            StreamNUCAMap::getNumRows());

  IrregularAlignField indField = decodeIrregularAlign(align.elemOffset);

  const auto &alignToRegion = this->getRegionFromStartVAddr(align.vaddrB);
  for (Addr vaddr = region.vaddr; vaddr < endVAddr; vaddr += boxSize) {
    auto boxHops = this->computeIndirectBoxHops(tc, region, alignToRegion,
                                                indField, vaddr);
    regionHops.boxHops.emplace_back(std::move(boxHops));
  }

  return regionHops;
}

StreamNUCAManager::IndirectBoxHops StreamNUCAManager::computeIndirectBoxHops(
    ThreadContext *tc, const StreamRegion &region,
    const StreamRegion &alignToRegion, const IrregularAlignField &indField,
    Addr boxVAddr) {

  auto boxSize = this->indirectRemapBoxBytes;
  auto totalSize = region.elementSize * region.numElement;
  auto endVAddr = std::min(region.vaddr + totalSize, boxVAddr + boxSize);
  auto numBytes = endVAddr - boxVAddr;
  auto boxIdx = (boxVAddr - region.vaddr) / boxSize;
  auto boxPAddr = this->translate(boxVAddr);
  auto defaultNodeId = StreamNUCAMap::mapPAddrToNUMAId(boxPAddr);

  auto numRows = StreamNUCAMap::getNumRows();
  auto numCols = StreamNUCAMap::getNumCols();
  auto numBanks = numRows * numCols;

  char *boxData = reinterpret_cast<char *>(malloc(boxSize));
  tc->getVirtProxy().readBlob(boxVAddr, boxData, boxSize);

  indRegionBoxes++;
  indRegionElements += numBytes / region.elementSize;

  IndirectBoxHops boxHops(boxVAddr, boxPAddr, defaultNodeId, numBanks);

  for (int i = 0; i < numBytes; i += region.elementSize) {
    int64_t index = 0;
    if (indField.size == 4) {
      index = *reinterpret_cast<int32_t *>(boxData + i + indField.offset);
    } else if (indField.size == 8) {
      index = *reinterpret_cast<int64_t *>(boxData + i + indField.offset);
    } else {
      panic("[StreamNUCA] Invalid IndAlign %s ElementSize %d Field Offset %d "
            "Size %d.",
            region.name, region.elementSize, indField.offset, indField.size);
    }
    if (index < 0 || index >= alignToRegion.numElement) {
      panic("[StreamNUCA] %s InvalidIndex Box %d-%d Addr %#x/%#x %d not in %s "
            "NumElement %d.",
            region.name, boxIdx, i, boxVAddr, boxPAddr, index,
            alignToRegion.name, alignToRegion.numElement);
    }
    auto alignToVAddr = alignToRegion.vaddr + index * alignToRegion.elementSize;
    auto alignToPAddr = this->translate(alignToVAddr);
    auto alignToBank = StreamNUCAMap::getBank(alignToPAddr);

    // DPRINTF(StreamNUCAManager,
    //         "  Index %ld AlignToVAddr %#x AlignToPAddr %#x AlignToBank
    //         %d.\n", index, alignToVAddr, alignToPAddr, alignToBank);

    if (alignToBank < 0 || alignToBank >= boxHops.bankFreq.size()) {
      panic("[StreamNUCA] IndirectAlign %s -> %s Box %lu Index %ld Invalid "
            "AlignToBank %d.",
            region.name, alignToRegion.name, boxIdx, index, alignToBank);
    }
    boxHops.bankFreq.at(alignToBank)++;
    boxHops.totalElements++;

    // Accumulate the traffic hops for all NUMA nodes.
    for (int bankIdx = 0; bankIdx < numBanks; ++bankIdx) {
      auto hops = StreamNUCAMap::computeHops(alignToBank, bankIdx);
      boxHops.hops.at(bankIdx) += hops;
    }
  }

  boxHops.maxHops = boxHops.hops.front();
  boxHops.minHops = boxHops.hops.front();
  boxHops.maxHopsBankIdx = 0;
  boxHops.minHopsBankIdx = 0;
  for (int bankIdx = 1; bankIdx < numBanks; ++bankIdx) {
    auto hops = boxHops.hops.at(bankIdx);
    if (hops > boxHops.maxHops) {
      boxHops.maxHops = hops;
      boxHops.maxHopsBankIdx = bankIdx;
    }
    if (hops < boxHops.minHops) {
      boxHops.minHops = hops;
      boxHops.minHopsBankIdx = bankIdx;
    }
  }

  return boxHops;
}

void StreamNUCAManager::greedyAssignIndirectBoxes(
    IndirectRegionHops &regionHops) {

  const auto numRows = StreamNUCAMap::getNumRows();
  const auto numCols = StreamNUCAMap::getNumCols();

  for (uint64_t boxIdx = 0; boxIdx < regionHops.boxHops.size(); ++boxIdx) {
    auto &boxHops = regionHops.boxHops.at(boxIdx);

    auto minHops = boxHops.minHops;
    auto minHopsBankIdx = boxHops.minHopsBankIdx;

    /**
     * Sort by their difference between MaxHops and MinHops.
     */
    regionHops.addRemapBoxId(boxIdx, minHopsBankIdx);

    indRegionMemToLLCMinHops += minHops;
    indRegionMemMinBanks.sample(minHopsBankIdx, 1);

    if (Debug::StreamNUCAManager) {
      int32_t avgBankFreq = boxHops.totalElements / boxHops.bankFreq.size();
      std::stringstream freqMatrixStr;
      for (int row = 0; row < numRows; ++row) {
        for (int col = 0; col < numCols; ++col) {
          auto bank = row * numCols + col;
          freqMatrixStr << std::setw(6)
                        << (boxHops.bankFreq[bank] - avgBankFreq);
        }
        freqMatrixStr << '\n';
      }
      DPRINTF(StreamNUCAManager,
              "[StreamNUCA] IndRegion %s BoxIdx %lu AvgBankFreq %d Diff:\n%s.",
              regionHops.region.name, boxIdx, avgBankFreq, freqMatrixStr.str());
    }
  }

  if (Debug::StreamNUCAManager) {
    const auto &region = regionHops.region;
    DPRINTF(StreamNUCAManager,
            "[StreamNUCA] IndirectRegion %s Finish Greedy Assign:\n",
            region.name);
    for (int i = 0; i < regionHops.numBanks; ++i) {
      auto pages = regionHops.remapBoxIds.at(i).size();
      auto totalBoxes = regionHops.boxHops.size();
      auto ratio = static_cast<float>(pages) / static_cast<float>(totalBoxes);
      DPRINTF(StreamNUCAManager, "[StreamNUCA]     Bank %5d Boxes %8lu %3.2f\n",
              i, pages, ratio * 100);
    }
  }
}

void StreamNUCAManager::IndirectRegionHops::addRemapBoxId(uint64_t boxIdx,
                                                          int bankIdx) {
  /**
   * Sorted by their difference between MaxHops and MinHops.
   */
  auto &remapBoxIds = this->remapBoxIds.at(bankIdx);
  auto &remapBoxHops = this->boxHops.at(boxIdx);
  remapBoxHops.remapBankIdx = bankIdx;

  auto remapDiffHops = remapBoxHops.maxHops - remapBoxHops.minHops;
  auto iter = remapBoxIds.begin();
  while (iter != remapBoxIds.end()) {
    auto pageId = *iter;
    const auto &boxHops = this->boxHops.at(pageId);
    auto diffHops = boxHops.maxHops - boxHops.minHops;
    if (diffHops < remapDiffHops) {
      break;
    }
    ++iter;
  }
  remapBoxIds.insert(iter, boxIdx);
}

void StreamNUCAManager::rebalanceIndirectBoxes(IndirectRegionHops &regionHops) {

  auto remapBoxIdsCmp =
      [](const IndirectRegionHops::RemapBoxIdsPerBankT &A,
         const IndirectRegionHops::RemapBoxIdsPerBankT &B) -> bool {
    return A.size() < B.size();
  };

  auto isBalanced = [&regionHops, &remapBoxIdsCmp, this]() -> bool {
    auto minMaxPair =
        std::minmax_element(regionHops.remapBoxIds.begin(),
                            regionHops.remapBoxIds.end(), remapBoxIdsCmp);
    auto diff = minMaxPair.second->size() - minMaxPair.first->size();
    auto ratio = static_cast<float>(diff) /
                 static_cast<float>(regionHops.boxHops.size());
    const float threshold = this->indirectRebalanceThreshold;
    return ratio <= threshold;
  };

  auto selectPopNUMAIter =
      [&regionHops,
       &remapBoxIdsCmp]() -> IndirectRegionHops::RemapBoxIdsT::iterator {
    // For now always select the NUMAId with most pages.
    auto maxIter =
        std::max_element(regionHops.remapBoxIds.begin(),
                         regionHops.remapBoxIds.end(), remapBoxIdsCmp);
    return maxIter;
  };

  auto selectPushNUMAIter =
      [&regionHops,
       &remapBoxIdsCmp]() -> IndirectRegionHops::RemapBoxIdsT::iterator {
    // For now always select the NUMAId with least pages.
    auto minIter =
        std::min_element(regionHops.remapBoxIds.begin(),
                         regionHops.remapBoxIds.end(), remapBoxIdsCmp);
    return minIter;
  };

  while (!isBalanced()) {
    auto popIter = selectPopNUMAIter();
    auto pushIter = selectPushNUMAIter();

    auto boxIdx = popIter->back();
    popIter->pop_back();

    auto pushNUMANodeId = pushIter - regionHops.remapBoxIds.begin();
    regionHops.addRemapBoxId(boxIdx, pushNUMANodeId);
  }

  if (Debug::StreamNUCAManager) {
    const auto &region = regionHops.region;
    DPRINTF(StreamNUCAManager,
            "[StreamNUCA] IndirectRegion %s Finish Rebalance:\n", region.name);
    for (int i = 0; i < regionHops.numBanks; ++i) {
      auto boxes = regionHops.remapBoxIds.at(i).size();
      auto totalBoxes = regionHops.boxHops.size();
      auto ratio = static_cast<float>(boxes) / static_cast<float>(totalBoxes);
      DPRINTF(StreamNUCAManager,
              "[StreamNUCA]     NUMANode %5d Pages %8lu %3.2f\n", i, boxes,
              ratio * 100);
    }
  }
}

void StreamNUCAManager::relocateIndirectBoxes(
    ThreadContext *tc, const IndirectRegionHops &regionHops) {

  std::map<int, std::vector<int>> forwardFreq;

  for (uint64_t boxIdx = 0; boxIdx < regionHops.boxHops.size(); ++boxIdx) {

    const auto &boxHops = regionHops.boxHops.at(boxIdx);
    auto remapBankIdx = boxHops.remapBankIdx;
    auto defaultBankIdx = boxHops.defaultBankIdx;

    if (this->indirectRemapBoxBytes == 0) {
      /**
       * IndirectRemap is disabled, we just set remapNUMA = defaultNUMA.
       */
      remapBankIdx = defaultBankIdx;
    }

    this->indRegionMemToLLCDefaultHops += boxHops.hops.at(defaultBankIdx);
    this->indRegionMemToLLCRemappedHops += boxHops.hops.at(remapBankIdx);
    this->indRegionMemRemappedBanks.sample(remapBankIdx, 1);

    if (!forwardFreq.count(remapBankIdx)) {
      forwardFreq
          .emplace(std::piecewise_construct,
                   std::forward_as_tuple(remapBankIdx), std::forward_as_tuple())
          .first->second.resize(boxHops.bankFreq.size(), 0);
    }
    auto &freq = forwardFreq.at(remapBankIdx);
    for (auto i = 0; i < boxHops.bankFreq.size(); ++i) {
      freq[i] += boxHops.bankFreq[i];
    }

    if (remapBankIdx == defaultBankIdx) {
      continue;
    }

    this->relocateCacheLines(tc, boxHops.vaddr, boxHops.paddr,
                             this->indirectRemapBoxBytes, remapBankIdx);

    // this->reallocatePageAt(tc, boxHops.vaddr, boxHops.paddr,
    // remapNUMANodeId);
  }

  for (const auto &bankFreq : forwardFreq) {
    std::ostringstream os;
    os << std::setw(3) << bankFreq.first << " -> ";
    for (int i = 0; i < bankFreq.second.size(); ++i) {
      if (bankFreq.second.at(i) == 0) {
        continue;
      }
      os << std::setw(3) << i << std::setw(6) << bankFreq.second.at(i) << ' ';
    }
    DPRINTF(StreamNUCAManager, "[IndFwd]  %s\n", os.str());
  }
}

void StreamNUCAManager::relocateCacheLines(ThreadContext *tc, Addr vaddrLine,
                                           Addr paddrLine, int size,
                                           int bankIdx) {

  std::unordered_map<Addr, int> paddrLineRemap;
  for (auto offset = 0; offset < size; ++offset) {
    Addr paddr = paddrLine + offset;
    paddrLineRemap.emplace(paddr, bankIdx);
  }
  StreamNUCAMap::overridePAddrToBank(paddrLineRemap);
}

void StreamNUCAManager::reallocatePageAt(ThreadContext *tc, Addr pageVAddr,
                                         Addr pagePAddr, int numaNode) {

  auto pTable = this->process->pTable;
  auto pageSize = pTable->getPageSize();

  char *pageData = reinterpret_cast<char *>(malloc(pageSize));

  tc->getVirtProxy().readBlob(pageVAddr, pageData, pageSize);

  auto oldNUMANode = StreamNUCAMap::mapPAddrToNUMAId(pagePAddr);
  /**
   * Try to allocate a page at selected bank. Remap the vaddr to the new paddr
   * by setting clobber flag (which will destroy the old mapping). Then copy
   * the data.
   */
  int allocPages = 0;
  int allocNUMANodeId = 0;
  auto newPagePAddr = NUMAPageAllocator::allocatePageAt(
      tc->getProcessPtr()->system, numaNode, allocPages, allocNUMANodeId);

  indRegionRemapPages++;
  indRegionAllocPages += allocPages;

  bool clobber = true;
  pTable->map(pageVAddr, newPagePAddr, pageSize, clobber);
  tc->getVirtProxy().writeBlob(pageVAddr, pageData, pageSize);

  // Return the old page to the allocator.
  NUMAPageAllocator::returnPage(pagePAddr, oldNUMANode);

  free(pageData);
}

void StreamNUCAManager::groupDirectRegionsByAlign() {
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
      DPRINTF(StreamNUCAManager, "[AlignGroup] Union %#x %#x.\n", align.vaddrA,
              align.vaddrB);
    }
  }

  for (const auto &entry : unionFindParent) {
    /**
     * Ignore all irregular regions when contructing groups.
     */
    const auto &region = this->getRegionFromStartVAddr(entry.first);
    if (region.isIrregular) {
      continue;
    }
    auto root = find(entry.first);
    this->directRegionAlignGroupVAddrMap
        .emplace(std::piecewise_construct, std::forward_as_tuple(root),
                 std::forward_as_tuple())
        .first->second.emplace_back(entry.first);
  }

  for (auto &entry : this->directRegionAlignGroupVAddrMap) {
    auto &group = entry.second;
    // Sort for simplicity.
    std::sort(group.begin(), group.end());
  }
}

void StreamNUCAManager::computeCachedElements() {

  const auto totalBanks =
      StreamNUCAMap::getNumRows() * StreamNUCAMap::getNumCols();
  const auto llcNumSets = StreamNUCAMap::getCacheNumSet();
  const auto llcAssoc = StreamNUCAMap::getCacheAssoc();
  const auto llcBlockSize = StreamNUCAMap::getCacheBlockSize();
  const auto llcBankSize = llcNumSets * llcAssoc * llcBlockSize;
  /**
   * Let's reserve 1MB of LLC size for other data.
   */
  const auto reservedLLCSize = 1024 * 1024;
  const auto totalLLCSize = llcBankSize * totalBanks - reservedLLCSize;

  for (auto &entry : this->directRegionAlignGroupVAddrMap) {
    auto &group = entry.second;

    /**
     * First we estimate how many data can be cached.
     * NOTE: If a region has non-zero non-self alignment, we assume the
     * offset is the unused data, e.g. first layer of hotspot3D.powerIn.
     * This is different than homogeneous case:
     * A [--- Cached --- | --- Uncached ---]
     * B [--- Cached --- | --- Uncached ---]
     * C [--- Cached --- | --- Uncached ---]
     *
     * Now we have some extra bytes:
     * A [        --- Cached --- | --- Uncached ---]
     * B [        --- Cached --- | --- Uncached ---]
     * C - Extra [--- Cached --- | --- Uncached ---]
     *
     * For A and B
     *  CachedElementsA = (TotalLLCSize + Extra) / TotalElementSize
     * For C
     *  CachedElementsC = CachedElementsA - Extra / ElementCSize
     *
     */
    auto totalElementSize = 0;
    auto totalSize = 0ul;
    auto extraSize = 0ul;
    auto getExtraSize = [](const StreamRegion &region) -> uint64_t {
      auto extraSize = 0ul;
      for (const auto &align : region.aligns) {
        if (align.vaddrA != align.vaddrB && align.elemOffset > 0) {
          if (extraSize != 0 && align.elemOffset != extraSize) {
            panic("Region %s Multi-ExtraSize %lu %lu.", region.name, extraSize,
                  align.elemOffset);
          }
          extraSize = align.elemOffset;
        }
      }
      return extraSize;
    };
    for (auto startVAddr : group) {
      const auto &region = this->getRegionFromStartVAddr(startVAddr);
      totalElementSize += region.elementSize;
      totalSize += region.elementSize * region.numElement;
      extraSize += getExtraSize(region);
    }

    if (this->directRegionFitPolicy == DirectRegionFitPolicy::DROP &&
        totalSize > totalLLCSize) {
      for (auto iter = group.begin(), end = group.end(); iter != end; ++iter) {
        auto startVAddr = *iter;
        auto &region = this->getRegionFromStartVAddr(startVAddr);
        if (region.name == "gap.pr_push.out_neigh_index" ||
            region.name == "rodinia.hotspot3D.powerIn" ||
            region.name == "rodinia.hotspot.power" ||
            region.name == "rodinia.pathfinder.wall") {
          totalElementSize -= region.elementSize;
          totalSize -= region.elementSize * region.numElement;
          extraSize -= getExtraSize(region);
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

    uint64_t cachedElements = (totalLLCSize + extraSize) / totalElementSize;

    DPRINTF(StreamNUCAManager,
            "[AlignGroup] Analyzing Group %#x NumRegions %d ExtraSize %lu "
            "TotalElementSize %d CachedElements %lu.\n",
            group.front(), group.size(), extraSize, totalElementSize,
            cachedElements);
    for (auto vaddr : group) {
      auto &region = this->getRegionFromStartVAddr(vaddr);
      auto extraSize = getExtraSize(region);
      auto regionCachedElements =
          cachedElements - extraSize / region.elementSize;
      DPRINTF(StreamNUCAManager,
              "[AlignGroup]   Region %#x Elements %lu ExtraSize %lu Cached "
              "%.2f%%.\n",
              vaddr, region.numElement, extraSize,
              static_cast<float>(regionCachedElements) /
                  static_cast<float>(region.numElement) * 100.f);
      region.cachedElements = std::min(regionCachedElements, region.numElement);
    }
  }
}

void StreamNUCAManager::computeCacheSetNUCA() {

  /**
   * Compute the StartSet for arrays.
   * NOTE: We ignore indirect regions, as they will be remapped at page
   * granularity.
   */

  const auto totalBanks =
      StreamNUCAMap::getNumRows() * StreamNUCAMap::getNumCols();
  const auto llcNumSets = StreamNUCAMap::getCacheNumSet();
  const auto llcBlockSize = StreamNUCAMap::getCacheBlockSize();

  for (auto &entry : this->directRegionAlignGroupVAddrMap) {
    auto &group = entry.second;

    auto totalElementSize = 0;
    auto totalSize = 0ul;
    for (auto startVAddr : group) {
      const auto &region = this->getRegionFromStartVAddr(startVAddr);
      totalElementSize += region.elementSize;
      totalSize += region.elementSize * region.numElement;
    }

    DPRINTF(
        StreamNUCAManager,
        "[CacheSet] Analyzing Group %#x NumRegions %d TotalElementSize %d.\n",
        group.front(), group.size(), totalElementSize);

    auto startSet = 0;
    for (auto startVAddr : group) {
      const auto &region = this->getRegionFromStartVAddr(startVAddr);

      auto startPAddr = this->translate(startVAddr);
      auto &rangeMap = StreamNUCAMap::getRangeMapByStartPAddr(startPAddr);
      rangeMap.startSet = startSet;

      auto cachedElements = region.cachedElements;

      auto cachedBytes = cachedElements * region.elementSize;
      auto usedSets = cachedBytes / (llcBlockSize * totalBanks);

      DPRINTF(StreamNUCAManager,
              "[CacheSet] Range %s %#x ElementSize %d CachedElements %lu "
              "StartSet %d UsedSet %d.\n",
              region.name, region.vaddr, region.elementSize, cachedElements,
              startSet, usedSets);
      startSet = (startSet + usedSets) % llcNumSets;
    }
  }
}

void StreamNUCAManager::computeCacheSetPUM() {

  const auto llcNumSets = StreamNUCAMap::getCacheNumSet();
  const auto llcBlockSize = StreamNUCAMap::getCacheBlockSize();
  const auto llcArraysPerWay = StreamNUCAMap::getCacheParams().arrayPerWay;

  auto startSet = 0;
  for (const auto &entry : this->startVAddrRegionMap) {
    const auto &startVAddr = entry.first;
    const auto &region = entry.second;

    auto startPAddr = this->translate(startVAddr);
    auto &rangeMap = StreamNUCAMap::getRangeMapByStartPAddr(startPAddr);
    if (!rangeMap.isStreamPUM) {
      continue;
    }
    rangeMap.startSet = startSet;

    auto elemSize = region.elementSize;
    auto usedBytesPerWay = elemSize * llcArraysPerWay * rangeMap.vBitlines;
    auto usedSets = usedBytesPerWay / llcBlockSize;

    DPRINTF(StreamNUCAManager,
            "[CacheSet] Range %s %#x ElementSize %d UsedBytesPerWay %lu "
            "StartSet %d UsedSet %d.\n",
            region.name, region.vaddr, region.elementSize, usedBytesPerWay,
            startSet, usedSets);

    assert(startSet + usedSets <= llcNumSets && "LLC Sets overflow.");
    startSet = (startSet + usedSets) % llcNumSets;
  }
}

void StreamNUCAManager::computeCacheSet() {
  if (this->enablePUM) {
    this->computeCacheSetPUM();
  } else {
    this->computeCacheSetNUCA();
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

  /**
   * If the region has user-defined interleave, use it.
   * Check that there are no alignment defined.
   */
  if (region.userDefinedProperties.count(RegionProperty::INTERLEAVE)) {
    if (!region.aligns.empty()) {
      panic("Range %s has both aligns and user-defined interleave.",
            region.name);
    }
    return region.userDefinedProperties.at(RegionProperty::INTERLEAVE) *
           region.elementSize;
  }

  auto numRows = StreamNUCAMap::getNumRows();
  auto numCols = StreamNUCAMap::getNumCols();
  auto numBanks = numRows * numCols;

  auto defaultWrapAroundBytes = defaultInterleave * numBanks;
  auto defaultColWrapAroundBytes = defaultInterleave * numCols;

  for (const auto &align : region.aligns) {
    const auto &alignToRegion = this->getRegionFromStartVAddr(align.vaddrB);

    auto elementOffset = align.elemOffset;
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
      if ((elementOffset < region.arraySizes.front())) {
        // Align along the inner-most dimension is considered implicitly done.
        DPRINTF(StreamNUCAManager, "Range %s %#x Self Aligned.\n", region.name,
                region.vaddr);
      } else if ((bytesOffset % defaultWrapAroundBytes) == 0) {
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
      } else if (bytesOffset < defaultColWrapAroundBytes &&
                 (defaultColWrapAroundBytes % bytesOffset) == 0) {
        // Try to align with one row.
        interleave =
            (bytesOffset * defaultInterleave) / defaultColWrapAroundBytes;
        DPRINTF(StreamNUCAManager,
                "Range %s %#x Self Aligned To Row Interleave %lu = %lu * %lu / "
                "%lu.\n",
                region.name, region.vaddr, interleave, bytesOffset,
                defaultInterleave, defaultColWrapAroundBytes);
        if (interleave != 128 && interleave != 256 && interleave != 512) {
          panic("Weird Interleave Found: Range %s %#x SelfAlign ElemOffset %lu "
                "BytesOffset %lu Intrlv %llu.\n",
                region.name, region.vaddr, align.elemOffset, bytesOffset,
                interleave);
        }
      } else {
        panic("Not Support Yet: Range %s %#x Self Align ElemOffset %lu "
              "ByteOffset %lu.\n",
              region.name, region.vaddr, align.elemOffset, bytesOffset);
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
  if (region.name.find("rodinia.pathfinder.") == 0 ||
      region.name.find("rodinia.hotspot.") == 0 ||
      region.name.find("rodinia.hotspot.") == 0 ||
      region.name.find("rodinia.srad_v2.") == 0 ||
      region.name.find("rodinia.srad_v3.") == 0 ||
      region.name.find("gap.pr_push") == 0 ||
      region.name.find("gap.bfs_push") == 0 ||
      region.name.find("gap.sssp") == 0 ||
      region.name.find("gap.pr_pull") == 0 ||
      region.name.find("gap.bfs_pull") == 0) {
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

void StreamNUCAManager::markRegionCached(Addr regionVAddr) {
  if (!this->enabledNUCA) {
    return;
  }
  const auto &region = this->getRegionFromStartVAddr(regionVAddr);
  Addr regionPAddr;
  if (!this->process->pTable->translate(regionVAddr, regionPAddr)) {
    panic("Failed to translate RegionVAddr %#x.\n", regionVAddr);
  }
  auto &nucaMapEntry = StreamNUCAMap::getRangeMapByStartPAddr(regionPAddr);
  nucaMapEntry.isCached = true;
  DPRINTF(StreamNUCAManager, "[StreamNUCA] Region %s Marked Cached.\n",
          region.name);
}

StreamNUCAManager::IrregularAlignField
StreamNUCAManager::decodeIrregularAlign(int64_t irregularAlign) {
  assert(irregularAlign < 0 && "This is not IrregularAlign.");

  const int SIZE_BITWIDTH = 8;
  const int SIZE_MASK = (1 << SIZE_BITWIDTH) - 1;
  const int OFFSET_BITWIDTH = 8;
  const int OFFSET_MASK = (1 << OFFSET_BITWIDTH) - 1;

  int64_t raw = -irregularAlign;

  int typeRaw = raw >> 16;
  IrregularAlignField::TypeE type = IrregularAlignField::TypeE::Indirect;
  if (typeRaw == 1) {
    type = IrregularAlignField::PtrChase;
  }
  int32_t offset = (raw >> SIZE_BITWIDTH) & OFFSET_MASK;
  int32_t size = raw & SIZE_MASK;
  return IrregularAlignField(type, offset, size);
}

bool StreamNUCAManager::canRemapDirectRegionPUM(const StreamRegion &region) {
  auto pumHWConfig = StreamNUCAMap::getPUMHWConfig();

  auto bitlines = pumHWConfig.array_cols;
  if (region.numElement < bitlines || region.numElement % bitlines != 0) {
    DPRINTF(
        StreamNUCAManager,
        "[StreamPUM] Region %s NumElem %llu not compatible with Bitlines %ld.",
        region.name, region.numElement, bitlines);
    return false;
  }
  /**
   * A heuristic to avoid mapping some arrays since they should never be mapped
   * to PUM.
   * TODO: Add pseudo-instructions to pass in this information.
   */
  if (region.userDefinedProperties.count(RegionProperty::USE_PUM) &&
      region.userDefinedProperties.at(RegionProperty::USE_PUM) == 0) {
    DPRINTF(StreamNUCAManager, "[StreamPUM] Region %s Manually Disabled PUM.\n",
            region.name);
    return false;
  }
  return true;
}

int64_t StreamNUCAManager::getVirtualBitlinesForPUM(
    const std::vector<Addr> &pumRegionVAddrs) {

  int64_t vBitlines = 0;
  for (auto regionVAddr : pumRegionVAddrs) {
    auto &region = this->getRegionFromStartVAddr(regionVAddr);
    vBitlines = std::max(vBitlines, this->getVirtualBitlinesForPUM(region));
  }

  return vBitlines;
}

int64_t
StreamNUCAManager::getVirtualBitlinesForPUM(const StreamRegion &region) {

  /**
   * When the total elements in the region is less than the total available
   * bitlines, we just map to the number of physical bitlines.
   *
   * However, when the total elements in the region is more than available
   * bitlines, we introduce the concept of virtual bitlines, and we may increase
   * the virtual bitlines beyond the actual physical bitlines, so that we make
   * sure that the number of tiles is within the number of SRAM arrays.
   *
   * When virtual bitlines is more than physical bitlines, it means that the
   * tile will be wrap around, and in PUMEngine, we charge multiple latency to
   * perform the bit-serial logic.
   */

  auto pumHWConfig = StreamNUCAMap::getPUMHWConfig();

  auto bitlines = pumHWConfig.array_cols;
  auto totalBitlines = pumHWConfig.get_total_arrays() * pumHWConfig.array_cols;

  auto totalElems = region.numElement;
  auto ratio = (totalElems + totalBitlines - 1) / totalBitlines;

  if (ratio > 2 || ratio < 1) {
    panic("[StreamPUM] Region %s TotalElem %lu / Bitlines %ld = %lu Illegal.",
          region.name, totalElems, totalBitlines, ratio);
  }

  auto vBitlines = ratio * bitlines;

  DPRINTF(StreamNUCAManager, "[StreamPUM] Region %s vBitlines %ld.\n",
          region.name, vBitlines);
  return vBitlines;
}

void StreamNUCAManager::remapDirectRegionPUM(const StreamRegion &region,
                                             int64_t vBitlines) {
  if (!this->isPAddrContinuous(region)) {
    panic("[StreamPUM] Region %s %#x PAddr is not continuous.", region.name,
          region.vaddr);
  }
  assert(this->canRemapDirectRegionPUM(region) && "Can not Map to PUM.");
  auto startVAddr = region.vaddr;
  auto startPAddr = this->translate(startVAddr);

  auto endPAddr = startPAddr + region.elementSize * region.numElement;

  auto dimensions = region.arraySizes.size();

  AffinePattern::IntVecT arraySizes = region.arraySizes;
  AffinePattern::IntVecT tileSizes(dimensions, 1);

  /**
   * We want to search for aligned dimensions from this region or it's
   * AlignedToRegion, and try to tile for those aligned dimensions.
   */
  auto alignDims = this->getAlignDimsForDirectRegion(region);
  auto numAlignDims = alignDims.size();
  assert(numAlignDims > 0 && "No AlignDims.");

  if (numAlignDims == 1) {
    /**
     * Just align to one dimension.
     * Pick the minimum of:
     *  bitlines, arraySize, userDefinedTileSize (if defined).
     * NOTE: UserDefinedTileSize is ignored if forceTilingDim is set.
     *
     * Then -- if there is more space, try to map the next dimension.
     *
     */
    auto alignDim = alignDims.front();
    auto arraySize = arraySizes.at(alignDim);

    auto alignDimTileSize = std::min(vBitlines, arraySize);
    if (!this->forcePUMTilingSize.empty() && arraySizes.size() > 1) {
      // Only force PUMTilingInnerSize when we have multi-dim array.
      alignDimTileSize =
          std::min(this->forcePUMTilingSize.front(), alignDimTileSize);
    }
    if (this->forcePUMTilingDim == "none" &&
        region.userDefinedProperties.count(
            RegionProperty::PUM_TILE_SIZE_DIM0)) {
      auto userDefinedTileSize =
          region.userDefinedProperties.at(RegionProperty::PUM_TILE_SIZE_DIM0);
      if (userDefinedTileSize < alignDimTileSize) {
        alignDimTileSize = userDefinedTileSize;
      }
    }

    tileSizes.at(alignDim) = alignDimTileSize;

    if (alignDimTileSize < vBitlines) {
      /**
       * We have more bitlines than this dimension. Try to fill in upper/lower
       * dimension.
       */
      assert(vBitlines % alignDimTileSize == 0);
      auto ratio = vBitlines / alignDimTileSize;
      if (alignDim + 1 < dimensions) {
        auto s = arraySizes.at(alignDim + 1);
        assert(s >= ratio);
        assert(s % ratio == 0);
        tileSizes.at(alignDim + 1) = ratio;
      } else if (alignDim > 0) {
        auto s = arraySizes.at(alignDim - 1);
        assert(s >= ratio);
        assert(s % ratio == 0);
        tileSizes.at(alignDim - 1) = ratio;
      }
    }
  } else if (numAlignDims == 2) {
    // Just try to get square root of bitlines?
    if (this->enablePUMTiling) {
      auto &x = tileSizes.at(alignDims.at(0));
      auto &y = tileSizes.at(alignDims.at(1));

      bool tileSizeChosen = false;
      if (!tileSizeChosen && !this->forcePUMTilingSize.empty()) {
        x = std::min(this->forcePUMTilingSize.front(),
                     arraySizes.at(alignDims.at(0)));
        y = vBitlines / x;
        tileSizeChosen = true;
      }
      if (!tileSizeChosen &&
          region.userDefinedProperties.count(RegionProperty::REDUCE_DIM)) {

        /**
         * We add a special case for reducing over inner dim.
         * This is used to balance the stream and pum reduction,
         * e.g. mm_inner.
         */

        auto reduceDim =
            region.userDefinedProperties.at(RegionProperty::REDUCE_DIM);
        assert(alignDims.at(0) == reduceDim);
        auto reduceDimArraySize = arraySizes.at(reduceDim);

        if (reduceDimArraySize <= vBitlines) {
          // Favor pum reduction.
          x = reduceDimArraySize;
        } else {
          // From profiling we know the ratio.
          x = std::min(vBitlines, 64l);
        }
        y = vBitlines / x;
        tileSizeChosen = true;
      }

      if (!tileSizeChosen &&
          region.userDefinedProperties.count(RegionProperty::BROADCAST_DIM)) {

        /**
         * We add a special case for reducing over inner dim.
         * This is used to balance the stream and pum reduction,
         * e.g. mm_inner.
         */

        auto broadcastDim =
            region.userDefinedProperties.at(RegionProperty::BROADCAST_DIM);
        assert(alignDims.at(0) == broadcastDim);
        auto broadcastDimArraySize = arraySizes.at(broadcastDim);

        if (broadcastDimArraySize > vBitlines) {
          // Favor small tiling sizes on inner broadcast dim to increase
          // bandwidth.
          x = std::min(vBitlines, 4l);
          y = vBitlines / x;
          tileSizeChosen = true;
        }
      }

      if (!tileSizeChosen && arraySizes.at(alignDims.at(0)) <= 128) {

        /**
         * Special rule when the inner dimension is very small. We heuristically
         * pick 64 as the inner tile size as it helps confine the data move
         * within two leaves of H-tree.
         */
        x = std::min(vBitlines, 64l);
        y = vBitlines / x;
        tileSizeChosen = true;
      }

      if (!tileSizeChosen) {
        // Default case.
        // Balance x and y and favor y when tied.
        x = vBitlines;
        y = 1;
        while (y < x) {
          y *= 2;
          x /= 2;
        }
        tileSizeChosen = true;
      }
    } else {
      // Tiling is not enabled, however, we tile to handle the case when dim0 <
      // bitlines.
      auto &x = tileSizes.at(0);
      auto &y = tileSizes.at(1);
      x = vBitlines;
      y = 1;
      auto size0 = arraySizes.at(0);
      if (size0 < vBitlines) {
        assert(vBitlines % size0 == 0);
        x = size0;
        y = vBitlines / size0;
      }
    }
  } else if (dimensions == 3) {
    if (this->enablePUMTiling) {
      auto &x = tileSizes.at(alignDims.at(0));
      auto &y = tileSizes.at(alignDims.at(1));
      auto &z = tileSizes.at(alignDims.at(2));
      if (!this->forcePUMTilingSize.empty()) {
        x = std::min(this->forcePUMTilingSize.front(),
                     arraySizes.at(alignDims.at(0)));
        if (this->forcePUMTilingSize.size() > 1) {
          // Force tiling the second dimension.
          y = std::min(this->forcePUMTilingSize.at(1),
                       arraySizes.at(alignDims.at(1)));
          z = vBitlines / x / y;
        } else {
          y = vBitlines / x;
          z = 1;
          while (z * 2 < y) {
            y /= 2;
            z *= 2;
          }
        }
      } else {
        /**
         * We try to sellect the middle of the design space.
         * Prioritize balance the outer dim.
         */
        auto sx = arraySizes.at(alignDims.at(0));
        auto sy = arraySizes.at(alignDims.at(1));
        auto sz = arraySizes.at(alignDims.at(2));
        auto chooseMiddleTileSize = [](int64_t min, int64_t max) -> int64_t {
          std::vector<int64_t> validTileSizes;
          int64_t x = 1;
          while (x <= max) {
            if (x >= min) {
              validTileSizes.push_back(x);
            }
            x *= 2;
          }
          assert(!validTileSizes.empty());
          return validTileSizes.at(validTileSizes.size() / 2);
        };
        {
          auto zTileMin = std::max(vBitlines / (sx * sy), 1l);
          auto zTileMax = std::min(vBitlines, sz);
          assert(zTileMax > zTileMin);
          z = chooseMiddleTileSize(zTileMin, zTileMax);
          auto yTileMin = std::max(vBitlines / (sx * z), 1l);
          auto yTileMax = std::min(vBitlines / (z), sy);
          assert(yTileMax > yTileMin);
          y = chooseMiddleTileSize(yTileMin, yTileMax);
          x = vBitlines / (y * z);
        }
        // x = vBitlines;
        // y = 1;
        // z = 1;
        // while (y * 4 < x) {
        //   x /= 4;
        //   y *= 2;
        //   z *= 2;
        // }
      }
    } else {
      // Tiling is not enabled, however, we tile to handle the case when dim0 <
      // bitlines.
      auto &x = tileSizes.at(0);
      auto &y = tileSizes.at(1);
      auto &z = tileSizes.at(2);
      x = vBitlines;
      y = 1;
      z = 1;
      auto size0 = arraySizes.at(0);
      if (size0 < vBitlines) {
        assert(vBitlines % size0 == 0);
        x = size0;
        y = vBitlines / size0;
        z = 1;
      }
    }
  } else {
    panic("[StreamPUM] Region %s too many dimensions.", region.name);
  }

  for (auto dim = 0; dim < dimensions; ++dim) {
    auto arraySize = arraySizes[dim];
    auto tileSize = tileSizes[dim];
    if (arraySize < tileSize) {
      panic("[StreamPUM] Region %s Dim %d %ld < %ld.", region.name, dim,
            arraySize, tileSize);
    }
    if (arraySize % tileSize != 0) {
      panic("[StreamPUM] Region %s Dim %d %ld %% %ld != 0.", region.name, dim,
            arraySize, tileSize);
    }
  }

  auto pumTile = AffinePattern::construct_canonical_tile(tileSizes, arraySizes);
  auto elemBits = region.elementSize * 8;
  auto startWordline = StreamNUCAMap::RangeMap::InvalidWordline;

  StreamNUCAMap::addRangeMap(startPAddr, endPAddr, pumTile, elemBits,
                             startWordline, vBitlines);
  DPRINTF(StreamNUCAManager,
          "[StreamPUM] Map %s PAddr %#x ElemBit %d StartWdLine %d Tile %s.\n",
          region.name, startPAddr, elemBits, startWordline, pumTile);

  if (region.userDefinedProperties.count(RegionProperty::PUM_NO_INIT) &&
      region.userDefinedProperties.at(RegionProperty::PUM_NO_INIT) == 1) {
    this->markRegionCached(region.vaddr);
  }
}

std::vector<int>
StreamNUCAManager::getAlignDimsForDirectRegion(const StreamRegion &region) {

  auto dimensions = region.arraySizes.size();
  std::vector<int> ret;

  // Force tiling on certain dimension.
  if (this->forcePUMTilingDim != "none") {
    if (this->forcePUMTilingDim == "inner") {
      ret.push_back(0);
    } else if (this->forcePUMTilingDim == "outer") {
      ret.push_back(dimensions - 1);
    } else {
      panic("Illegal forced PUM tiling dim %s.", this->forcePUMTilingDim);
    }
    return ret;
  }

  if (region.userDefinedProperties.count(RegionProperty::PUM_TILE_SIZE_DIM0)) {
    /**
     * User specified dim0 tile size. So we just set align to dim0.
     */
    ret.push_back(0);
    return ret;
  }

  for (const auto &align : region.aligns) {
    if (align.vaddrB == region.vaddr) {
      // Found a self align.
      auto elemOffset = align.elemOffset;
      auto arrayDimSize = 1;
      auto foundDim = false;
      for (auto dim = 0; dim < dimensions; ++dim) {
        if (elemOffset == arrayDimSize) {
          // Found the dimension.
          ret.push_back(dim);
          foundDim = true;
          break;
        }
        arrayDimSize *= region.arraySizes.at(dim);
      }
      if (!foundDim) {
        panic("[StreamNUCA] Region %s SelfAlign %ld Not Align to Dim.",
              region.name, align.elemOffset);
      }
    } else {
      // This array aligns to some other array.
      const auto &alignToRegion = this->getRegionFromStartVAddr(align.vaddrB);
      assert(alignToRegion.arraySizes.size() == dimensions &&
             "Mismatch in AlignedArray Dimensions.");
      return this->getAlignDimsForDirectRegion(alignToRegion);
    }
  }
  /**
   * By default we align to the first dimension.
   */
  if (ret.empty()) {
    ret.push_back(0);
  }
  return ret;
}