#ifndef __GEM_FORGE_STREAM_NUCA_MAP_HH__
#define __GEM_FORGE_STREAM_NUCA_MAP_HH__

#include "base/types.hh"

#include <unordered_map>
#include <vector>

/**
 * This is in charge of mapping physical addresses to some banks.
 * It is implemented as a global static object to be easily accessed.
 *
 * There are two types of mapping.
 * 1. Range-based mapping: like a segment.
 * 2. Page-based mapping: like a virtual pages.
 */

class StreamNUCAMap {
public:
  static void initializeTopology(int numRows, int numCols);

  static int getNumRows() {
    assert(topologyInitialized && "Topology has not initialized");
    return numRows;
  }
  static int getNumCols() {
    assert(topologyInitialized && "Topology has not initialized");
    return numCols;
  }

  static void addRangeMap(Addr startPAddr, Addr endPAddr, uint64_t interleave,
                          int startBank);

  static int getBank(Addr paddr);

private:
  static bool topologyInitialized;
  static int numRows;
  static int numCols;

  struct RangeMap {
    Addr startPAddr;
    Addr endPAddr;
    uint64_t interleave;
    int startBank;
    RangeMap(Addr _startPAddr, Addr _endPAddr, uint64_t _interleave,
             int _startBank)
        : startPAddr(_startPAddr), endPAddr(_endPAddr), interleave(_interleave),
          startBank(_startBank) {}
  };

  static std::vector<RangeMap> rangeMaps;
};

#endif