#ifndef __GEM_FORGE_STREAM_NUCA_MANAGER_HH__
#define __GEM_FORGE_STREAM_NUCA_MANAGER_HH__

#include "sim/process.hh"

#include <map>
#include <vector>

class StreamNUCAManager {
public:
  StreamNUCAManager(Process *_process, bool _enabled)
      : process(_process), enabled(_enabled) {}

  /**
   * We panic on copy. Required for process clone.
   */
  StreamNUCAManager(const StreamNUCAManager &other);
  StreamNUCAManager &operator=(const StreamNUCAManager &other);

  void defineRegion(Addr start, uint64_t elementSize, uint64_t numElement);
  void defineAlign(Addr A, Addr B, uint64_t elementOffset);
  void remap();

  struct StreamAlign {
    Addr vaddrA;
    Addr vaddrB;
    int64_t elementOffset;
    StreamAlign(Addr _vaddrA, Addr _vaddrB, int64_t _elementOffset)
        : vaddrA(_vaddrA), vaddrB(_vaddrB), elementOffset(_elementOffset) {}
  };

  struct StreamRegion {
    Addr vaddr;
    uint64_t elementSize;
    uint64_t numElement;
    StreamRegion(Addr _vaddr, uint64_t _elementSize, uint64_t _numElement)
        : vaddr(_vaddr), elementSize(_elementSize), numElement(_numElement) {}

    std::vector<StreamAlign> aligns;
  };

  const StreamRegion &getContainingStreamRegion(Addr vaddr) const;

private:
  Process *process;
  bool enabled;

  std::map<Addr, StreamRegion> startVAddrRegionMap;

  StreamRegion &getRegionFromStartVAddr(Addr vaddr);

  bool isPAddrContinuous(const StreamRegion &region);

  Addr translate(Addr vaddr);

  uint64_t determineInterleave(const StreamRegion &region);
  void computeCacheSet();
};

#endif