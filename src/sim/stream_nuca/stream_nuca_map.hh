#ifndef __GEM_FORGE_STREAM_NUCA_MAP_HH__
#define __GEM_FORGE_STREAM_NUCA_MAP_HH__

#include "base/addr_range.hh"
#include "base/types.hh"
#include "mem/ruby/common/MachineID.hh"

#include <map>

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
  static void initializeCache(int blockSize, int numSet, int assoc);

  struct NonUniformNode {
    int routerId;
    MachineID machineId;
    AddrRange addrRange;
    std::vector<int> handleBanks;
    NonUniformNode(int _routerId, MachineID _machineId,
                   const AddrRange &_addrRange,
                   const std::vector<int> &_handleBanks)
        : routerId(_routerId), machineId(_machineId), addrRange(_addrRange),
          handleBanks(_handleBanks) {}
  };
  using NonUniformNodeVec = std::vector<NonUniformNode>;
  static void addNonUniformNode(int routerId, MachineID machineId,
                                const AddrRange &addrRange,
                                const std::vector<int> &handleBanks);
  static const NonUniformNodeVec &getNUMANodes() { return numaNodes; }
  static const NonUniformNode &mapPAddrToNUMANode(Addr paddr);
  static int mapPAddrToNUMARouterId(Addr paddr);
  static int mapPAddrToNUMAId(Addr paddr);
  static int64_t computeHops(int64_t bankA, int64_t bankB);

  static int getNumRows() {
    assert(topologyInitialized && "Topology has not initialized");
    return numRows;
  }
  static int getNumCols() {
    assert(topologyInitialized && "Topology has not initialized");
    return numCols;
  }
  static int getCacheBlockSize() {
    assert(cacheInitialized && "Cache has not initialized");
    return cacheBlockSize;
  }
  static int getCacheNumSet() {
    assert(cacheInitialized && "Cache has not initialized");
    return cacheNumSet;
  }
  static int getCacheAssoc() {
    assert(cacheInitialized && "Cache has not initialized");
    return cacheAssoc;
  }

  struct RangeMap {
    Addr startPAddr;
    Addr endPAddr;
    uint64_t interleave;
    int startBank;
    int startSet;
    RangeMap(Addr _startPAddr, Addr _endPAddr, uint64_t _interleave,
             int _startBank, int _startSet)
        : startPAddr(_startPAddr), endPAddr(_endPAddr), interleave(_interleave),
          startBank(_startBank), startSet(_startSet) {}
  };

  static void addRangeMap(Addr startPAddr, Addr endPAddr, uint64_t interleave,
                          int startBank, int startSet);

  static RangeMap &getRangeMapByStartPAddr(Addr startPAddr);
  static RangeMap *getRangeMapContaining(Addr paddr);
  static int getBank(Addr paddr);
  static int getSet(Addr paddr);

private:
  static bool topologyInitialized;
  static int numRows;
  static int numCols;
  static bool cacheInitialized;
  static int cacheBlockSize;
  static int cacheNumSet;
  static int cacheAssoc;

  static NonUniformNodeVec numaNodes;

  static std::map<Addr, RangeMap> rangeMaps;
};

#endif