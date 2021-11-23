#ifndef __GEM_FORGE_STREAM_NUCA_MANAGER_HH__
#define __GEM_FORGE_STREAM_NUCA_MANAGER_HH__

#include "sim/process.hh"

#include <map>
#include <vector>

class StreamNUCAManager {
public:
  StreamNUCAManager(Process *_process, bool _enabled,
                    float _indirectPageRemapThreshold)
      : process(_process), enabled(_enabled),
        indirectPageRemapThreshold(_indirectPageRemapThreshold) {}

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
          numElement(_numElement), isIndirect(false) {}

    std::vector<StreamAlign> aligns;
  };

  const StreamRegion &getContainingStreamRegion(Addr vaddr) const;
  int getNumStreamRegions() const { return this->startVAddrRegionMap.size(); }

private:
  Process *process;
  const bool enabled;
  const float indirectPageRemapThreshold;

  std::map<Addr, StreamRegion> startVAddrRegionMap;

  StreamRegion &getRegionFromStartVAddr(Addr vaddr);

  bool isPAddrContinuous(const StreamRegion &region);

  Addr translate(Addr vaddr);

  void remapRegion(ThreadContext *tc, const StreamRegion &region);

  void remapDirectRegion(const StreamRegion &region);
  uint64_t determineInterleave(const StreamRegion &region);

  void remapIndirectRegion(ThreadContext *tc, const StreamRegion &region);
  void remapIndirectPage(ThreadContext *tc, const StreamRegion &region,
                         const StreamRegion &alignToRegion, Addr pageVAddr);
  int64_t computeHopsAndFreq(const StreamRegion &region,
                             const StreamRegion &alignToRegion, Addr pageVAddr,
                             int64_t numBytes, char *pageData,
                             std::vector<int32_t> &alignToBankFrequency);

  void computeCacheSet();

  /**
   * Stats.
   */
  static bool statsRegsiterd;
  static Stats::ScalarNoReset indRegionPages;
  static Stats::ScalarNoReset indRegionElements;
  static Stats::ScalarNoReset indRegionAllocPages;
  static Stats::ScalarNoReset indRegionRemapPages;
  static Stats::ScalarNoReset indRegionMemToLLCDefaultHops;
  static Stats::DistributionNoReset indRegionMemOptimizedBanks;

  static Stats::ScalarNoReset indRegionMemToLLCRemappedHops;
  static Stats::DistributionNoReset indRegionMemRemappedBanks;

  static Stats::ScalarNoReset indRegionMemToLLCFinalHops;
  static Stats::DistributionNoReset indRegionMemFinalBanks;
};

#endif