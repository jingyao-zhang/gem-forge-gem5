#include "stream_placement_manager.hh"

#include "coalesced_stream.hh"

#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "mem/cache/cache.hh"

StreamPlacementManager::StreamPlacementManager(LLVMTraceCPU *_cpu,
                                               StreamEngine *_se)
    : cpu(_cpu), se(_se) {

  Cache *L2, *L1D;
  for (auto so : this->cpu->getSimObjectList()) {
    if (so->name() == "system.l2") {
      // L2 cache.
      L2 = dynamic_cast<Cache *>(so);
    } else if (so->name() == "system.cpu.dcache") {
      // L1 data cache.
      L1D = dynamic_cast<Cache *>(so);
    }
  }

  assert(L1D != nullptr);
  assert(L2 != nullptr);
  this->caches.push_back(L1D);
  this->caches.push_back(L2);
  this->lookupLatency.push_back(L1D->getLookupLatency());
  this->lookupLatency.push_back(L2->getLookupLatency());
}

bool StreamPlacementManager::access(Stream *stream, Addr paddr, int packetSize,
                                    Stream::StreamMemAccess *memAccess) {

  auto coalescedStream = dynamic_cast<CoalescedStream *>(stream);
  if (coalescedStream == nullptr) {
    // So far we only consider coalesced streams.
    return false;
  }

  auto placeCacheLevel = this->whichCacheLevelToPlace(coalescedStream);

  if (placeCacheLevel == 0) {
    // L1 cache is not handled by us.
    return false;
  }

  bool hasHit = false;
  Cycles latency = Cycles(1);

  for (int cacheLevel = 0; cacheLevel < this->caches.size(); ++cacheLevel) {
    auto cache = this->caches[cacheLevel];
    auto isHitThisLevel = this->isHit(cache, paddr);
    if (cacheLevel < placeCacheLevel) {
      if (isHitThisLevel) {
        if (!hasHit) {
          // This is still where we sent the request.
          latency = this->lookupLatency[cacheLevel];
        } else {
          // We have already been hit.
        }
      } else {
        // Does not hit in this level.
      }
    } else if (cacheLevel == placeCacheLevel) {
      if (isHitThisLevel) {
        // Hit in what we expected.
        if (!hasHit) {
          latency = this->lookupLatency[cacheLevel];
        } else {
        }
      } else {
        // Build up the lookup latency.
        latency += this->lookupLatency[cacheLevel];
      }
    } else {
      if (isHitThisLevel) {
        if (!hasHit) {
          latency += this->lookupLatency[cacheLevel];
        } else {
          // Already hit.
        }
      } else {
        latency += this->lookupLatency[cacheLevel];
      }
    }
    hasHit = hasHit || isHitThisLevel;
  }

  // No matter what, we do a functional access to the placed level to
  // bring it in.
  auto pkt = this->createPacket(paddr, packetSize);
  if (placeCacheLevel < this->caches.size()) {
    // This is still cache.
    auto cache = this->caches[placeCacheLevel];
    cache->functionalAccess(pkt, true);
  } else {
    // The stream is placed in the memory.
  }

  // Schedule the response to this packet.
  this->scheduleResponse(latency, memAccess, pkt);

  return true;
}

bool StreamPlacementManager::isHit(Cache *cache, Addr paddr) const {
  return cache->inCache(paddr, false);
}

PacketPtr StreamPlacementManager::createPacket(Addr paddr, int size) const {
  RequestPtr req = new Request(paddr, size, 0, Request::funcMasterId);
  PacketPtr pkt;
  uint8_t *pkt_data = new uint8_t[req->getSize()];
  pkt = Packet::createRead(req);
  pkt->dataDynamic(pkt_data);
  return pkt;
}

void StreamPlacementManager::scheduleResponse(
    Cycles latency, Stream::StreamMemAccess *memAccess, PacketPtr pkt) {

  EventWrapper<ResponseEvent, &ResponseEvent::recvResponse> responseEvent(
      new ResponseEvent(cpu, memAccess, pkt));
  cpu->schedule(responseEvent, cpu->clockEdge(latency));
}

size_t
StreamPlacementManager::whichCacheLevelToPlace(CoalescedStream *stream) const {
  auto footprint = stream->getFootprint(cpu->system->cacheLineSize());
  for (size_t cacheLevel = 0; cacheLevel < this->caches.size(); ++cacheLevel) {
    auto cache = this->caches[cacheLevel];
    if (footprint < (cache->getCacheSize() / cpu->system->cacheLineSize())) {
      return cacheLevel;
    }
  }
  assert(!this->caches.empty());
  return this->caches.size() - 1;
}