#ifndef __GEM_FORGE_NUMA_PAGE_ALLOCATOR_HH__
#define __GEM_FORGE_NUMA_PAGE_ALLOCATOR_HH__

#include "base/types.hh"

#include <deque>
#include <vector>

class System;

/**
 * Represents a global page allocator to explore locality in numa nodes.
 */
class NUMAPageAllocator {
public:
  /**
   * Allocate one page at Node. Notice that NodeId is not the router id but the
   * directory id.
   * If node interleaving is smaller than a page size, this will allocate at the
   * bank containing the start of that physical address.
   * @return physical page address at that node.
   * @return totalAllocPages: total pages allocated this time.
   * @return allocNodeId: which node this page gets allocated.
   */
  static Addr allocatePageAt(System *system, int nodeId, int &totalAllocPages,
                             int &allocNodeId);

private:
  static bool initialized;
  static System *system;
  static int pageBytes;
  static int interleaveBytes;
  static int nodesPerPage;

  static std::vector<std::deque<Addr>> freePages;

  static void initialize(System *system);

  static int getQueueId(int nodeId) { return nodeId - (nodeId % nodesPerPage); }

  /**
   * Allocate pages for one round.
   * @return number of pages allocated.
   */
  static int allocateOneRound();
};

#endif