#ifndef __GEM_FORGE_STREAM_NUCA_MANAGER_HH__
#define __GEM_FORGE_STREAM_NUCA_MANAGER_HH__

#include "sim/process.hh"

#include <map>
#include <vector>

class StreamNUCAManager {
public:
  StreamNUCAManager(Process *_process, bool _enabled,
                    const std::string &_directRegionFitPolicy,
                    float _indirectPageRemapThreshold);

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
                    uint64_t elementSize, uint64_t numElement);

  /**
   * Negative element offset will specify some indirect alignment.
   */
  enum StreamNUCAIndirectAlignment {
    STREAM_NUCA_IND_ALIGN_EVERY_ELEMENT = -1,
  };
  void defineAlign(Addr A, Addr B, int64_t elementOffset);
  void remap(ThreadContext *tc);
  uint64_t getCachedBytes(Addr start);

  struct StreamAlign {
    Addr vaddrA;
    Addr vaddrB;
    int64_t elementOffset;
    StreamAlign(Addr _vaddrA, Addr _vaddrB, int64_t _elementOffset)
        : vaddrA(_vaddrA), vaddrB(_vaddrB), elementOffset(_elementOffset) {}
  };

  struct StreamRegion {
    std::string name;
    Addr vaddr;
    uint64_t elementSize;
    uint64_t numElement;
    bool isIndirect;
    StreamRegion(const std::string &_name, Addr _vaddr, uint64_t _elementSize,
                 uint64_t _numElement)
        : name(_name), vaddr(_vaddr), elementSize(_elementSize),
          numElement(_numElement), isIndirect(false),
          cachedElements(_numElement) {}

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
  int getNumStreamRegions() const { return this->startVAddrRegionMap.size(); }

private:
  Process *process;
  const bool enabled;
  enum DirectRegionFitPolicy {
    CROP,
    DROP,
  };
  DirectRegionFitPolicy directRegionFitPolicy;
  const float indirectPageRemapThreshold;

  std::map<Addr, StreamRegion> startVAddrRegionMap;

  bool isPAddrContinuous(const StreamRegion &region);

  Addr translate(Addr vaddr);

  void remapRegion(ThreadContext *tc, StreamRegion &region);

  void remapDirectRegion(const StreamRegion &region);
  uint64_t determineInterleave(const StreamRegion &region);
  int determineStartBank(const StreamRegion &region, uint64_t interleave);

  void remapIndirectRegion(ThreadContext *tc, StreamRegion &region);
  int64_t computeHopsAndFreq(const StreamRegion &region,
                             const StreamRegion &alignToRegion, Addr pageVAddr,
                             int64_t numBytes, char *pageData,
                             std::vector<int32_t> &alignToBankFrequency);

  void computeCacheSet();

  struct IndirectPageHops {
    const Addr pageVAddr;
    const Addr defaultPagePAddr;
    const int defaultNUMANodeId;
    std::vector<int64_t> hops;
    std::vector<int64_t> bankFreq;
    int64_t maxHops = -1;
    int64_t minHops = -1;
    int maxHopsNUMANodeId = -1;
    int minHopsNUMANodeId = -1;
    int64_t totalElements = 0;
    /**
     * Remap decisions.
     */
    int remapNUMANodeId;
    IndirectPageHops(Addr _pageVAddr, Addr _defaultPagePAddr,
                     int _defaultNUMANodeId, int _numMemNodes, int _numBanks)
        : pageVAddr(_pageVAddr), defaultPagePAddr(_defaultPagePAddr),
          defaultNUMANodeId(_defaultNUMANodeId) {
      this->hops.resize(_numMemNodes, 0);
      this->bankFreq.resize(_numBanks, 0);
    }
  };

  struct IndirectRegionHops {
    const StreamRegion &region;
    const int numMemNodes;
    std::vector<IndirectPageHops> pageHops;
    /**
     * Remap decisions.
     * They are sorted by their bias ratio.
     */
    using RemapPageIdsPerNUMANodeT = std::vector<uint64_t>;
    using RemapPageIdsT = std::vector<RemapPageIdsPerNUMANodeT>;
    RemapPageIdsT remapPageIds;
    IndirectRegionHops(const StreamRegion &_region, int _numMemNodes)
        : region(_region), numMemNodes(_numMemNodes) {
      this->remapPageIds.resize(this->numMemNodes);
    }
    void addRemapPageId(uint64_t pageId, int NUMANodeId);
  };

  /**
   * Collect the hops and frequency stats for indirect regions.
   */
  IndirectRegionHops computeIndirectRegionHops(ThreadContext *tc,
                                               const StreamRegion &region);
  IndirectPageHops computeIndirectPageHops(ThreadContext *tc,
                                           const StreamRegion &region,
                                           const StreamRegion &alignToRegion,
                                           Addr pageVAddr);

  /**
   * Just greedily assign pages to the NUMA node Id with the lowest traffic.
   */
  void greedyAssignIndirectPages(IndirectRegionHops &regionHops);

  /**
   * Try to rebalance page remap.
   */
  void rebalanceIndirectPages(IndirectRegionHops &regionHops);

  /**
   * Relocate pages according to the remap decision.
   */
  void relocateIndirectPages(ThreadContext *tc,
                             const IndirectRegionHops &regionHops);

  /**
   * Stats.
   */
  static bool statsRegsiterd;
  static Stats::ScalarNoReset indRegionPages;
  static Stats::ScalarNoReset indRegionElements;
  static Stats::ScalarNoReset indRegionAllocPages;
  static Stats::ScalarNoReset indRegionRemapPages;

  static Stats::ScalarNoReset indRegionMemToLLCDefaultHops;

  static Stats::ScalarNoReset indRegionMemToLLCMinHops;
  static Stats::DistributionNoReset indRegionMemMinBanks;

  static Stats::ScalarNoReset indRegionMemToLLCRemappedHops;
  static Stats::DistributionNoReset indRegionMemRemappedBanks;
};

#endif