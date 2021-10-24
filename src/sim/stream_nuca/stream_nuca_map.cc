#include "stream_nuca_map.hh"

#include "base/trace.hh"

#include "debug/StreamNUCAMap.hh"

bool StreamNUCAMap::topologyInitialized = false;
int StreamNUCAMap::numRows = 0;
int StreamNUCAMap::numCols = 0;
bool StreamNUCAMap::cacheInitialized = false;
int StreamNUCAMap::cacheBlockSize = 0;
int StreamNUCAMap::cacheNumSet = 0;
int StreamNUCAMap::cacheAssoc = 0;
std::map<Addr, StreamNUCAMap::RangeMap> StreamNUCAMap::rangeMaps;

void StreamNUCAMap::initializeTopology(int numRows, int numCols) {
  if (topologyInitialized) {
    if (numCols != StreamNUCAMap::numCols ||
        numRows != StreamNUCAMap::numRows) {
      panic("Mismatch in NumRows %d != %d or NumCols %d != %d.", numRows,
            StreamNUCAMap::numRows, numCols, StreamNUCAMap::numCols);
    }
    return;
  } else {
    StreamNUCAMap::numCols = numCols;
    StreamNUCAMap::numRows = numRows;
    StreamNUCAMap::topologyInitialized = true;
  }
}

void StreamNUCAMap::initializeCache(int blockSize, int numSet, int assoc) {
  if (cacheInitialized) {
    if (blockSize != StreamNUCAMap::cacheBlockSize ||
        numSet != StreamNUCAMap::cacheNumSet ||
        assoc != StreamNUCAMap::cacheAssoc) {
      panic("Mismatch in BlockSize %d != %d or NumSet %d != %d or Assoc %d != "
            "%d.",
            blockSize, StreamNUCAMap::cacheBlockSize, numSet,
            StreamNUCAMap::cacheNumSet, assoc, StreamNUCAMap::cacheAssoc);
    }
    return;
  } else {
    StreamNUCAMap::cacheBlockSize = blockSize;
    StreamNUCAMap::cacheNumSet = numSet;
    StreamNUCAMap::cacheAssoc = assoc;
    StreamNUCAMap::cacheInitialized = true;
  }
}

void StreamNUCAMap::addRangeMap(Addr startPAddr, Addr endPAddr,
                                uint64_t interleave, int startBank,
                                int startSet) {
  // Simple sanity check that not overlap with existing ranges.
  for (const auto &entry : rangeMaps) {
    const auto &range = entry.second;
    if (range.startPAddr >= endPAddr || range.endPAddr <= startPAddr) {
      continue;
    }
    panic("Overlap in StreamNUCA RangeMap [%#x, %#x) [%#x, %#x).", startPAddr,
          endPAddr, range.startPAddr, range.endPAddr);
  }
  DPRINTF(StreamNUCAMap, "Add PAddrRangeMap [%#x, %#x) %% %lu + %d.\n",
          startPAddr, endPAddr, interleave, startBank);
  rangeMaps.emplace(std::piecewise_construct, std::forward_as_tuple(startPAddr),
                    std::forward_as_tuple(startPAddr, endPAddr, interleave,
                                          startBank, startSet));
}

StreamNUCAMap::RangeMap &
StreamNUCAMap::getRangeMapByStartPAddr(Addr startPAddr) {
  auto iter = rangeMaps.find(startPAddr);
  if (iter == rangeMaps.end()) {
    panic("Failed to find Range by StartPAddr %#x.", startPAddr);
  }
  return iter->second;
}

StreamNUCAMap::RangeMap *StreamNUCAMap::getRangeMapContaining(Addr paddr) {
  auto iter = rangeMaps.upper_bound(paddr);
  if (iter == rangeMaps.begin()) {
    return nullptr;
  }
  --iter;
  auto &range = iter->second;
  if (range.endPAddr <= paddr) {
    return nullptr;
  }
  return &range;
}

int StreamNUCAMap::getBank(Addr paddr) {
  if (auto *range = getRangeMapContaining(paddr)) {
    auto interleave = range->interleave;
    auto startPAddr = range->startPAddr;
    auto endPAddr = range->endPAddr;
    auto startBank = range->startBank;
    auto diffPAddr = paddr - startPAddr;
    auto bank = startBank + diffPAddr / interleave;
    bank = bank % (getNumRows() * getNumCols());
    DPRINTF(StreamNUCAMap,
            "Map PAddr %#x in [%#x, %#x) %% %lu + StartBank(%d) to Bank %d of "
            "%dx%d.\n",
            paddr, startPAddr, endPAddr, interleave, startBank, bank,
            getNumRows(), getNumCols());
    return bank;
  }
  return -1;
}

int StreamNUCAMap::getSet(Addr paddr) {
  if (auto *range = getRangeMapContaining(paddr)) {
    auto interleave = range->interleave;
    auto startPAddr = range->startPAddr;
    auto endPAddr = range->endPAddr;
    auto startSet = range->startSet;
    auto diffPAddr = paddr - startPAddr;
    auto globalBankInterleave = interleave * getNumCols() * getNumRows();
    /**
     * Skip the line bits and bank bits.
     */
    auto localBankOffset = diffPAddr % interleave;
    auto globalBankOffset = diffPAddr / globalBankInterleave;

    auto setNum = (globalBankOffset * (interleave / getCacheBlockSize())) +
                  localBankOffset / getCacheBlockSize();

    auto finalSetNum = (setNum + startSet) % getCacheNumSet();

    DPRINTF(StreamNUCAMap,
            "Map PAddr %#x in [%#x, %#x) %% %lu + StartSet(%d) "
            "to Set %d of %d.\n",
            paddr, startPAddr, endPAddr, interleave, startSet, finalSetNum,
            getCacheNumSet());

    return finalSetNum;
  }
  return -1;
}