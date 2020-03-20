#include "se_page_walker.hh"
#include "base/trace.hh"

#include "debug/TLB.hh"

namespace X86ISA {

SEPageWalker::SEPageWalker(const std::string &_name, Cycles _latency,
                           uint32_t _numContext)
    : myName(_name), latency(_latency), numContext(_numContext) {}

void SEPageWalker::regStats() {
  this->accesses.name(this->name() + ".accesses").desc("PageWalker accesses");
  this->hits.name(this->name() + ".hits")
      .desc("PageWalker hits on infly walks");
  this->waits.name(this->name() + ".waits")
      .desc("PageWalker waits on available context");
}

void SEPageWalker::clearReadyStates(Cycles curCycle) {
  for (auto iter = this->inflyState.begin(), end = this->inflyState.end();
       iter != end;) {
    if (iter->readyCycle < curCycle) {
      assert(this->pageVAddrToStateMap.erase(iter->pageVAddr) &&
             "Failed to find the state in PageVAddrToStateMap.");
      iter = this->inflyState.erase(iter);
    } else {
      // This state is not ready yet.
      break;
    }
  }
}

Cycles SEPageWalker::walk(Addr pageVAddr, Cycles curCycle) {
  this->clearReadyStates(curCycle);

  this->accesses++;
  DPRINTF(TLB, "Walk PageTable %#x.\n", pageVAddr);

  // Check if we have a parallel miss to the same page.
  auto iter = this->pageVAddrToStateMap.find(pageVAddr);
  if (iter != this->pageVAddrToStateMap.end()) {
    /**
     * Just add one cycle latency for parallel miss.
     */
    this->hits++;
    DPRINTF(TLB, "Hit %#x, Latency %s.\n", pageVAddr,
            iter->second->readyCycle - curCycle + Cycles(1));
    return iter->second->readyCycle - curCycle + Cycles(1);
  }

  /**
   * Try to allocate a new state for this miss.
   * First we have to get the available context.
   */
  auto prevContextIter = this->inflyState.rbegin();
  auto prevContextEnd = this->inflyState.rend();
  int i = 0;
  while (i < this->numContext && prevContextIter != prevContextEnd) {
    prevContextIter++;
  }

  Cycles allocateCycle = curCycle;
  if (prevContextIter != prevContextEnd) {
    // We have to wait until this state to be done before we can
    // serve this translation.
    allocateCycle = prevContextIter->readyCycle;
    this->waits++;
  }

  // Allocate it.
  auto readyCycle = allocateCycle + this->latency;
  this->inflyState.emplace_back(pageVAddr, readyCycle);
  this->pageVAddrToStateMap.emplace(pageVAddr, &(this->inflyState.back()));

  // We should only return the difference.
  return readyCycle - curCycle;
}

} // namespace X86ISA