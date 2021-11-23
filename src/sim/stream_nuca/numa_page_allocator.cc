#include "numa_page_allocator.hh"

#include "sim/system.hh"
#include "stream_nuca_map.hh"

#include "base/trace.hh"

#include "debug/NUMAPageAllocator.hh"

bool NUMAPageAllocator::initialized = false;
System *NUMAPageAllocator::system = nullptr;
int NUMAPageAllocator::pageBytes = 0;
int NUMAPageAllocator::interleaveBytes = 0;
int NUMAPageAllocator::nodesPerPage = 0;

std::vector<std::deque<Addr>> NUMAPageAllocator::freePages;

Addr NUMAPageAllocator::allocatePageAt(System *system, int nodeId,
                                       int &totalAllocPages, int &allocNodeId) {
  initialize(system);

  totalAllocPages = 0;

  assert(nodeId < freePages.size() && "Invalid NodeId.");
  auto queueId = getQueueId(nodeId);
  auto &queue = freePages.at(queueId);

  // AllocNodeId is the same as the QueueId.
  allocNodeId = queueId;

  if (queue.empty()) {
    totalAllocPages = allocateOneRound();
    assert(!queue.empty() && "Queue still empty after allocating one round.");
  }

  auto pagePAddr = queue.front();
  queue.pop_front();
  DPRINTF(NUMAPageAllocator,
          "[NUMAPageAlloc] Dequeue Page %#x NodeId %d QueueId %d.\n", pagePAddr,
          nodeId, queueId);
  return pagePAddr;
}

void NUMAPageAllocator::returnPage(Addr pagePAddr, int nodeId) {

  assert(NUMAPageAllocator::initialized &&
         "Not intialized when returning a page.");

  assert(nodeId < freePages.size() && "Invalid NodeId.");
  auto queueId = getQueueId(nodeId);
  auto &queue = freePages.at(queueId);
  queue.push_front(pagePAddr);
  DPRINTF(NUMAPageAllocator,
          "[NUMAPageAlloc] Return Page %#x NodeId %d QueueId %d.\n", pagePAddr,
          nodeId, queueId);
}

void NUMAPageAllocator::initialize(System *system) {
  if (NUMAPageAllocator::initialized) {
    assert(system == NUMAPageAllocator::system &&
           "NUMAPageAllocator only works with single system.");
    return;
  }
  NUMAPageAllocator::initialized = true;
  NUMAPageAllocator::system = system;
  NUMAPageAllocator::pageBytes = system->getPageBytes();

  const auto &numaNodes = StreamNUCAMap::getNUMANodes();
  assert(!numaNodes.empty() && "Missing NUMANodes for NUMAPageAllocator.");
  const auto &firstNUMANode = numaNodes.front();
  assert(firstNUMANode.addrRange.interleaved() && "NUMANodes not Interleaved.");
  NUMAPageAllocator::interleaveBytes = firstNUMANode.addrRange.granularity();
  if (interleaveBytes > pageBytes) {
    panic("NUMA Interleave %d > Page %d.", interleaveBytes, pageBytes);
  }
  NUMAPageAllocator::nodesPerPage = pageBytes / interleaveBytes;
  if (nodesPerPage != 1 && nodesPerPage != 2 && nodesPerPage != 4) {
    panic("Unsupported NUMA Nodes per Page %d.", nodesPerPage);
  }

  DPRINTF(NUMAPageAllocator,
          "[NUMAPageAlloc] Initalized PageBytes %d IntrlvBytes %d NodesPerPage "
          "%d NumNodes %d.\n",
          pageBytes, interleaveBytes, nodesPerPage, numaNodes.size());

  for (int i = 0; i < numaNodes.size(); ++i) {
    freePages.emplace_back();
  }
}

int NUMAPageAllocator::allocateOneRound() {

  const auto &numaNodes = StreamNUCAMap::getNUMANodes();

  auto pagesPerRound = freePages.size() / nodesPerPage;
  auto startPAddr = system->allocPhysPages(pagesPerRound);

  for (int i = 0; i < pagesPerRound; ++i) {
    auto pagePAddr = startPAddr + i * pageBytes;
    // Enqueue this page to the free list.
    int selectedNUMANodeId = -1;
    for (int j = 0; j < numaNodes.size(); ++j) {
      const auto &numaNode = numaNodes[j];
      if (numaNode.addrRange.contains(pagePAddr)) {
        selectedNUMANodeId = j;
        break;
      }
    }
    assert(selectedNUMANodeId != -1 && "Failed to select NUMANode.");
    DPRINTF(NUMAPageAllocator,
            "[NUMAPageAlloc] Enqueue %dth Page %#x to Node %d.\n", i, pagePAddr,
            selectedNUMANodeId);

    auto queueId = getQueueId(selectedNUMANodeId);
    assert(queueId == selectedNUMANodeId && "QueueId != SelectedNUMANodeId.");
    freePages.at(queueId).push_back(pagePAddr);
  }
  return pagesPerRound;
}