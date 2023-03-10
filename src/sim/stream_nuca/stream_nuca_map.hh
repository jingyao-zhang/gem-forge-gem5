#ifndef __GEM_FORGE_STREAM_NUCA_MAP_HH__
#define __GEM_FORGE_STREAM_NUCA_MAP_HH__

#include "base/addr_range.hh"
#include "base/types.hh"
#include "mem/ruby/common/MachineID.hh"

#include "cpu/gem_forge/accelerator/stream/cache/pum/AffinePattern.hh"
#include "cpu/gem_forge/accelerator/stream/cache/pum/PUMHWConfiguration.hh"

#include <map>

namespace gem5 {

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

  struct CacheParams {
    int blockSize = 0;
    int numSet = 0;
    int assoc = 0;
    /**
     * These are SRAM PUM parameters.
     */
    int wordlines = 0;
    int bitlines = 0;
    int arrayTreeDegree = 0;
    int arrayTreeLeafBandwidth = 0;
    int arrayPerWay = 0;
    int wayTreeDegree = 0;
    bool operator==(const CacheParams &other) const {
      return blockSize == other.blockSize && numSet == other.numSet &&
             assoc == other.assoc && wordlines == other.wordlines &&
             bitlines == other.bitlines &&
             arrayTreeDegree == other.arrayTreeDegree &&
             arrayTreeLeafBandwidth == other.arrayTreeLeafBandwidth &&
             arrayPerWay == other.arrayPerWay &&
             wayTreeDegree == other.wayTreeDegree;
    }
    bool operator!=(const CacheParams &other) const {
      return !this->operator==(other);
    }
  };

  static void initializeCache(const CacheParams &cacheParams);

  struct NonUniformNode {
    int routerId;
    ruby::MachineID machineId;
    AddrRange addrRange;
    std::vector<int> handleBanks;
    NonUniformNode(int _routerId, ruby::MachineID _machineId,
                   const AddrRange &_addrRange,
                   const std::vector<int> &_handleBanks)
        : routerId(_routerId), machineId(_machineId), addrRange(_addrRange),
          handleBanks(_handleBanks) {}
  };
  using NonUniformNodeVec = std::vector<NonUniformNode>;
  static void addNonUniformNode(int routerId, ruby::MachineID machineId,
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
  static const CacheParams &getCacheParams() {
    assert(cacheInitialized && "Cache has not initialized");
    return cacheParams;
  }
  static int getCacheBlockSize() { return getCacheParams().blockSize; }
  static int getCacheNumSet() { return getCacheParams().numSet; }
  static int getCacheAssoc() { return getCacheParams().assoc; }

  struct RangeMap {
    /**
     * This is the key data structure to record a custom mapping from physical
     * addresses to LLC banks. There are two cases:
     *
     * 1. For normal StreamNUCA, we only care about the interleave between LLC
     * banks, and the formula is:
     *  bank = (startBank + (paddr - startPAddr) / interleave) % numBanks;
     * And we may also change the startSet to avoid set conflict.
     *
     * 2. For transposed StreamPUM, we need to know exactly bitline/wordline
     * location of the data. Specifically:
     *  a. We records the CanonicalTile pattern and its reverse.
     *    The CanonicalTile maps VirtualBitlineIdx to ElementIdx.
     *  b. Since we assume each tile is mapped to one SRAM array, we have to
     *    translate VirtualBitlineIdx to PhyscialBitlineIdx:
     *    TileSize <= BitlinePerArray
     *    PhysicalBitlineIdx = (VirtualBitlineIdx / TileSize) * BitlinePerArray
     *                       + (VirtualBitlineIdx % TileSize)
     *  c. Then we get the wordline and handles wrapping around.
     *    TotalBitlines = BitlinePerArray * TotalArrays
     *    Wraps = PhysicalBitlineIdx / TotalBitlines
     *    PhyscialWordlineIdx = StartWordline + Wraps * ElementSizeInBits
     *    PhysicalBitlineIdx = PhyscialBitlineIdx % TotalBitlines
     */
    Addr startPAddr;
    Addr endPAddr;
    bool isStreamPUM = false;
    bool isCached = false;
    /**
     * StreamNUCA mapping.
     */
    uint64_t interleave;
    int startBank;
    int startSet;
    /**
     * StreamPUM mapping.
     */
    AffinePattern pumTile;
    AffinePattern pumTileRev;
    int elementBits;
    static constexpr int InvalidWordline = -1;
    int startWordline;
    int vBitlines;

    RangeMap(Addr _startPAddr, Addr _endPAddr, uint64_t _interleave,
             int _startBank, int _startSet)
        : startPAddr(_startPAddr), endPAddr(_endPAddr), isStreamPUM(false),
          interleave(_interleave), startBank(_startBank), startSet(_startSet) {}

    RangeMap(Addr _startPAddr, Addr _endPAddr, const AffinePattern &_pumTile,
             int _elementBits, int _startWordline, int _vBitlines)
        : startPAddr(_startPAddr), endPAddr(_endPAddr), isStreamPUM(true),
          pumTile(_pumTile), pumTileRev(_pumTile.revert_canonical_tile()),
          elementBits(_elementBits), startWordline(_startWordline),
          vBitlines(_vBitlines) {}
  };

  // Default (no remap).
  static void addRangeMap(Addr startPAddr, Addr endPAddr);
  // Remap a region with customized interleave.
  static void addRangeMap(Addr startPAddr, Addr endPAddr, uint64_t interleave,
                          int startBank, int startSet);
  // Remap a region with PUM
  static void addRangeMap(Addr startPAddr, Addr endPAddr,
                          const AffinePattern &pumTile, int elementBits,
                          int startWordline, int vBitlines);

  static RangeMap *tryGetRangeMapByStartPAddr(Addr startPAddr);
  static RangeMap &getRangeMapByStartPAddr(Addr startPAddr);
  static RangeMap *getRangeMapContaining(Addr paddr);

  /**
   * Set the start wordline for the PUMRange. Notice that it will mark any
   * previous region mapped to that wordline as uncached.
   */
  static void setWordlineForRange(Addr startPAddr, int wordline, bool noClear);
  static void clearWordline(int wordline, Addr skipPAddr = 0);
  static void evictRange(RangeMap &range);

  static int getBank(Addr paddr);
  static int getSet(Addr paddr);

  /**
   * Represent a Location in the SRAM LLC.
   * Notice: all indexes are local to its parent level.
   */
  struct SRAMLocation {
    int bank = 0;
    int way = 0;
    int array = 0;
    int bitline = 0;
    int wordline = 0;
  };
  static SRAMLocation getPUMLocation(Addr paddr, const RangeMap &range);

  static PUMHWConfiguration getPUMHWConfig();

  static void
  overridePAddrToBank(const std::unordered_map<Addr, int> &overrideMap);

private:
  static bool topologyInitialized;
  static int numRows;
  static int numCols;
  static bool cacheInitialized;
  static CacheParams cacheParams;

  static NonUniformNodeVec numaNodes;

  static std::map<Addr, RangeMap> rangeMaps;
  static std::map<int, Addr> pumWordlineToRangeMap;

  static std::unordered_map<Addr, int> paddrLineToBankMap;

  static void checkOverlapRange(Addr startPAddr, Addr endPAddr);

  static int getNUCABank(Addr paddr, const RangeMap &range);
  static int getNUCASet(Addr paddr, const RangeMap &range);

  static int getPUMSet(Addr paddr, const RangeMap &range);
};

} // namespace gem5

#endif