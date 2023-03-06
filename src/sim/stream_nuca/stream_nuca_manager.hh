#ifndef __GEM_FORGE_STREAM_NUCA_MANAGER_HH__
#define __GEM_FORGE_STREAM_NUCA_MANAGER_HH__

#include "base/output.hh"
#include "sim/process.hh"

#include <map>
#include <vector>

namespace gem5 {

class StreamNUCAManager {
public:
  StreamNUCAManager(Process *_process, const ProcessParams *_params);

  /**
   * We panic on copy. Required for process clone.
   */
  StreamNUCAManager(const StreamNUCAManager &other);
  StreamNUCAManager &operator=(const StreamNUCAManager &other);

  /**
   * Register some stats.
   */
  void regStats();

  void defineRegion(const std::string &regionName, Addr start,
                    uint64_t elementSize,
                    const std::vector<int64_t> &arraySizes);

  /**
   * Allow the user to manually set some property of the region.
   */
  enum RegionProperty {
    // Manually overrite the interleaving (in elements).
    INTERLEAVE = 0,
    USE_PUM,
    PUM_NO_INIT,
    PUM_TILE_SIZE_DIM0,
    REDUCE_DIM,
    BROADCAST_DIM,
  };
  void setProperty(Addr start, uint64_t property, uint64_t value);

  /**
   * Negative element offset will specify some indirect alignment.
   *
   * To support arbitrary indirect field alignment, e.g. in weighted graph
   * edge.v is used for indirect access while edge.w is only for compute.
   * Suppose the indirect region has this data structure:
   * IndElement {
   *   int32_t out_v;
   *   int32_t weight;
   *   ...
   * };
   *
   * Then the indirect field offset is 0, with size 4.
   * We use eight bits for each, and the final alignment is:
   * - ((offset << 8) | size).
   */
  struct IrregularAlignField {
    enum TypeE {
      Indirect,
      PtrChase,
      CSRIndex,
    };
    const TypeE type;
    const int32_t indCountOffset = 0;
    const int32_t indCountSize = 0;
    const int32_t indOffset = 0;
    const int32_t indSize = 0;
    const int32_t indStride = 0;
    const int32_t ptrCountOffset = 0;
    const int32_t ptrCountSize = 0;
    const int32_t ptrOffset = 0;
    const int32_t ptrSize = 0;
    const int32_t ptrStride = 0;
    IrregularAlignField(TypeE _type, int32_t _indCountOffset,
                        int32_t _indCountSize, int32_t _indOffset,
                        int32_t _indSize, int32_t _indStride,
                        int32_t _ptrCountOffset, int32_t _ptrCountSize,
                        int32_t _ptrOffset, int32_t _ptrSize,
                        int32_t _ptrStride)
        : type(_type), indCountOffset(_indCountOffset),
          indCountSize(_indCountSize), indOffset(_indOffset), indSize(_indSize),
          indStride(_indStride), ptrCountOffset(_ptrCountOffset),
          ptrCountSize(_ptrCountSize), ptrOffset(_ptrOffset), ptrSize(_ptrSize),
          ptrStride(_ptrStride) {}
  };
  static IrregularAlignField decodeIrregularAlign(int64_t irregularAlign);
  void defineAlign(Addr A, Addr B, int64_t elementOffset);
  void remap(ThreadContext *tc);
  uint64_t getCachedBytes(Addr start);
  void markRegionCached(Addr regionVAddr);

  struct StreamAlign {
    Addr vaddrA;
    Addr vaddrB;
    int64_t elemOffset;
    StreamAlign(Addr _vaddrA, Addr _vaddrB, int64_t _elementOffset)
        : vaddrA(_vaddrA), vaddrB(_vaddrB), elemOffset(_elementOffset) {}
  };

  struct StreamRegion {
    std::string name;
    Addr vaddr;
    uint64_t elementSize;
    uint64_t numElement;
    std::vector<int64_t> arraySizes;
    bool isIrregular = false;
    bool isPtrChase = false;
    /**
     * Some user-defined properties.
     */
    using UserDefinedPropertyMap = std::map<RegionProperty, uint64_t>;
    UserDefinedPropertyMap userDefinedProperties;
    StreamRegion(const std::string &_name, Addr _vaddr, uint64_t _elementSize,
                 int64_t _numElement, const std::vector<int64_t> &_arraySizes)
        : name(_name), vaddr(_vaddr), elementSize(_elementSize),
          numElement(_numElement), arraySizes(_arraySizes),
          cachedElements(_numElement) {}

    std::string getRegionGroup() const {
      auto idx = this->name.find('/');
      if (idx != std::string::npos) {
        return this->name.substr(0, idx);
      } else {
        return this->name;
      }
    }

    std::vector<StreamAlign> aligns;
    /**
     * Results of remap.
     * 1. cacheNumElements: number of elements gets cached on chip. Default will
     * cache all elements.
     */
    uint64_t cachedElements;
  };

  StreamRegion &getRegionFromStartVAddr(Addr vaddr);
  StreamRegion &getRegionFromName(const std::string &name);
  const StreamRegion &getContainingStreamRegion(Addr vaddr) const;
  const StreamRegion *tryGetContainingStreamRegion(Addr vaddr) const;
  int getNumStreamRegions() const { return this->startVAddrRegionMap.size(); }

private:
  Process *process;
  const bool enabledMemStream;
  const bool enabledNUCA;
  const bool enablePUM;
  const bool enablePUMTiling;
  const std::string forcePUMTilingDim;
  const std::vector<int64_t> forcePUMTilingSize;
  enum DirectRegionFitPolicy {
    CROP,
    DROP,
  };
  DirectRegionFitPolicy directRegionFitPolicy;
  /**
   * Indirect regions are remapped at specified granularity (called box).
   * 0 to disable indirect region remap.
   */
  const int indirectRemapBoxBytes = 0;
  const float indirectRebalanceThreshold = 0.0f;
  const bool enableCSRReorder;

  std::map<Addr, StreamRegion> startVAddrRegionMap;

  bool isPAddrContinuous(const StreamRegion &region);

  /**
   * Helper function to make paddr of a region continuous.
   */
  void makeRegionPAddrContinuous(ThreadContext *tc, const StreamRegion &region);

  Addr translate(Addr vaddr);

  using AddrVecT = std::vector<Addr>;
  void remapRegions(ThreadContext *tc, const AddrVecT &regionVAddrs);

  bool canRemapDirectRegionPUM(const StreamRegion &region);
  int64_t getVirtualBitlinesForPUM(const std::vector<Addr> &pumRegionVAddrs);
  int64_t getVirtualBitlinesForPUM(const StreamRegion &region);
  void remapDirectRegionPUM(const StreamRegion &region, int64_t vBitlines);
  void remapDirectRegionNUCA(const StreamRegion &region);
  uint64_t determineInterleave(const StreamRegion &region);
  int determineStartBank(const StreamRegion &region, uint64_t interleave);

  void computeCachedElements();
  void computeCacheSet();
  void computeCacheSetNUCA();
  void computeCacheSetPUM();

  struct IndirectBoxHops {
    const Addr vaddr;
    const Addr paddr;
    const int defaultBankIdx;
    std::vector<int64_t> hops;
    std::vector<int64_t> bankFreq;
    int64_t maxHops = -1;
    int64_t minHops = -1;
    int maxHopsBankIdx = -1;
    int minHopsBankIdx = -1;
    int64_t totalElements = 0;
    /**
     * Remap decisions.
     */
    int remapBankIdx;
    IndirectBoxHops(Addr _vaddr, Addr _paddr, int _defaultBankIdx,
                    int _numBanks)
        : vaddr(_vaddr), paddr(_paddr), defaultBankIdx(_defaultBankIdx) {
      this->hops.resize(_numBanks, 0);
      this->bankFreq.resize(_numBanks, 0);
    }
  };

  const std::string &
  getRegionNameForIndirectBox(const IndirectBoxHops &box) const {
    return this->getContainingStreamRegion(box.vaddr).name;
  }

  struct IndirectRegionHops {
    const int numBanks;
    std::vector<IndirectBoxHops> boxHops;
    /**
     * Remap decisions.
     * They are sorted by their bias ratio.
     */
    using RemapBoxIdsPerBankT = std::vector<uint64_t>;
    using RemapBoxIdsT = std::vector<RemapBoxIdsPerBankT>;
    RemapBoxIdsT remapBoxIds;
    IndirectRegionHops(int _numBanks) : numBanks(_numBanks) {
      this->remapBoxIds.resize(this->numBanks);
    }
    void addRemapBoxId(uint64_t boxIdx, int bankIdx);
  };

  void remapPtrChaseRegion(ThreadContext *tc, StreamRegion &region);
  void remapIndirectRegion(ThreadContext *tc, StreamRegion &region,
                           IndirectRegionHops &regionHops);

  /**
   * Collect the hops and frequency stats for indirect regions.
   */
  void computeIndirectRegionHops(ThreadContext *tc, const StreamRegion &region,
                                 IndirectRegionHops &regionHops);
  IndirectBoxHops computeIndirectBoxHops(ThreadContext *tc,
                                         const StreamRegion &region,
                                         const StreamRegion &alignToRegion,
                                         const IrregularAlignField &indField,
                                         Addr boxVAddr);
  void computeIndirectHopsForOneElement(
      ThreadContext *tc, const StreamRegion &region,
      const StreamRegion &alignToRegion, const IrregularAlignField &indField,
      uint64_t boxIdx, Addr boxVAddr, Addr boxPAddr, int elemIdx,
      const char *elemData, IndirectBoxHops &boxHops);

  /**
   * Just greedily assign pages to the NUMA node Id with the lowest traffic.
   */
  void greedyAssignIndirectBoxes(IndirectRegionHops &regionHops);

  /**
   * Try to rebalance page remap.
   */
  void rebalanceIndirectBoxes(IndirectRegionHops &regionHops);

  /**
   * Relocate pages according to the remap decision.
   */
  void relocateIndirectBoxes(ThreadContext *tc,
                             const IndirectRegionHops &regionHops);
  /**
   * Estimate the CSR edge list migration.
   */
  struct CSREdgeListLine {
    Addr vaddr;
    int bank;
    CSREdgeListLine(Addr _vaddr, int _bank) : vaddr(_vaddr), bank(_bank) {}
  };
  void estimateCSRMigration(ThreadContext *tc, const StreamRegion &region);
  /**
   * Helper function to relocate a continuous range of physical lines.
   */
  void relocateCacheLines(ThreadContext *tc, Addr vaddrLine, Addr paddrLine,
                          int size, int bankIdx);
  /**
   * Helper function to remap a page via reallocation.
   *
   */
  void reallocatePageAt(ThreadContext *tc, Addr pageVAddr, Addr pagePAddr,
                        int numaNode);

  /**
   * Group direct regions by their alignment requirement.
   * Map from the root VAddr to a vector of VAddr.
   */
  std::map<Addr, std::vector<Addr>> directRegionAlignGroupVAddrMap;
  void groupDirectRegionsByAlign();

  /**
   * @brief Get the tiled dimensions for the stream region.
   *
   * @param region
   * @return std::vector<int>
   */
  std::vector<int> getAlignDimsForDirectRegion(const StreamRegion &region);

  /**
   * Stats.
   */
  static std::shared_ptr<StreamNUCAManager> singleton;

public:
  // There is only one StreamNUCAManager.
  static std::shared_ptr<StreamNUCAManager>
  initialize(Process *_process, const ProcessParams *_params);

private:
  bool statsRegisterd = false;
  statistics::ScalarNoReset indRegionBoxes;
  statistics::ScalarNoReset indRegionElements;
  statistics::ScalarNoReset indRegionAllocPages;
  statistics::ScalarNoReset indRegionRemapPages;
  statistics::ScalarNoReset indRegionMemToLLCDefaultHops;
  statistics::ScalarNoReset indRegionMemToLLCMinHops;
  statistics::DistributionNoReset indRegionMemMinBanks;
  statistics::ScalarNoReset indRegionMemToLLCRemappedHops;
  statistics::DistributionNoReset indRegionMemRemappedBanks;
  statistics::ScalarNoReset csrEdgeMigrations;
  statistics::ScalarNoReset csrEdgeMigrationHops;
  statistics::ScalarNoReset csrReorderEdgeMigrations;
  statistics::ScalarNoReset csrReorderEdgeMigrationHops;

  static OutputStream *log;
};

} // namespace gem5

#endif
