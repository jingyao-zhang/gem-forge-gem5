#include "stream_nuca_map.hh"

#include "base/trace.hh"

#include "debug/StreamNUCAMap.hh"

bool StreamNUCAMap::topologyInitialized = false;
int StreamNUCAMap::numRows = 0;
int StreamNUCAMap::numCols = 0;
std::vector<StreamNUCAMap::RangeMap> StreamNUCAMap::rangeMaps;

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

void StreamNUCAMap::addRangeMap(Addr startPAddr, Addr endPAddr,
                                uint64_t interleave, int startBank) {
  // Simple sanity check that not overlap with existing ranges.
  for (const auto &range : rangeMaps) {
    if (range.startPAddr >= endPAddr || range.endPAddr <= startPAddr) {
      continue;
    }
    panic("Overlap in StreamNUCA RangeMap [%#x, %#x) [%#x, %#x).", startPAddr,
          endPAddr, range.startPAddr, range.endPAddr);
  }
  DPRINTF(StreamNUCAMap, "Add PAddrRangeMap [%#x, %#x) %% %lu + %d.\n",
          startPAddr, endPAddr, interleave, startBank);
  rangeMaps.emplace_back(startPAddr, endPAddr, interleave, startBank);
}

int StreamNUCAMap::getBank(Addr paddr) {
  // Search in range maps.
  for (const auto &range : rangeMaps) {
    if (paddr >= range.startPAddr && paddr < range.endPAddr) {
      auto bank =
          range.startBank + (paddr - range.startPAddr) / range.interleave;
      bank = bank % (getNumRows() * getNumCols());
      DPRINTF(StreamNUCAMap,
              "Map PAddr %#x in [%#x, %#x) %% %lu + %d to Bank %d of %dx%d.\n",
              paddr, range.startPAddr, range.endPAddr, range.interleave,
              range.startBank, bank, getNumRows(), getNumCols());
      return bank;
    }
  }
  return -1;
}
