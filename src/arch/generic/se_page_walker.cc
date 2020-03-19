#include "se_page_walker.hh"

namespace X86ISA {

SEPageWalker::SEPageWalker(const std::string &_name, Cycles _latency,
                           uint32_t _numContext)
    : name(_name), latency(_latency), numContext(_numContext) {}

void SEPageWalker::regStats() {
  this->accesses.name(this->name + ".accesses").desc("PageWalker accesses");
  this->hits.name(this->name + ".hits").desc("PageWalker hits on infly walks");
  this->waits.name(this->name + ".waits")
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

  // Check if we have a parallel miss to the same page.
  auto iter = this->pageVAddrToStateMap.find(pageVAddr);
  if (iter != this->pageVAddrToStateMap.end()) {
    /**
     * Normally we would add some latency for this miss,
     * but I do not bother to do that, which would also
     * add some latency to the current miss and resort
     * the infly list.
     */
    this->hits++;
    return iter->second->readyCycle;
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