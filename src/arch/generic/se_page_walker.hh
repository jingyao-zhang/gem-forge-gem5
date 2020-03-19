#ifndef __ARCH_X86_SE_PAGE_WALKER_HH__
#define __ARCH_X86_SE_PAGE_WALKER_HH__

#include "base/statistics.hh"
#include "base/types.hh"

#include <list>
#include <map>

/**
 * This is a pretty much fake page walker used in SE mode to model
 * latency (only) for missing in TLB. It models:
 * 1. Contention on the TLB walker.
 * 2. Parallel misses on the same page.
 *
 * It does not model:
 * 1. Memory accesses to search the page table.
 * 2. Higher-level page table entries may be buffered in MMU.
 * 3. Squashed translation may release the walker.
 * 4. Variable page size.
 */

namespace X86ISA {
class SEPageWalker {
public:
  SEPageWalker(const std::string &_name, Cycles _latency, uint32_t _numContext);

  void regStats();

  /**
   * Start a walk.
   * @return The difference between curCycle and readyCycle.
   */
  Cycles walk(Addr pageVAddr, Cycles curCycle);

  Stats::Scalar accesses;
  Stats::Scalar hits;
  Stats::Scalar waits;

private:
  std::string name;
  const Cycles latency;
  const uint32_t numContext;

  struct State {
    Addr pageVAddr;
    Cycles readyCycle;
    State(Addr _pageVAddr, Cycles _readyCycle)
        : pageVAddr(_pageVAddr), readyCycle(_readyCycle) {}
  };

  /**
   * Infly states, sorted by ready cycles.
   */
  std::list<State> inflyState;
  std::map<Addr, State *> pageVAddrToStateMap;

  /**
   * Clear any infly states that are earlier than curCycle.
   */
  void clearReadyStates(Cycles curCycle);
};
} // namespace X86ISA

#endif