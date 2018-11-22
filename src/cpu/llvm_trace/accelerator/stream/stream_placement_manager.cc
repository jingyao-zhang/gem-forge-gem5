#include "stream_placement_manager.hh"

#include "coalesced_stream.hh"
#include "stream_engine.hh"

#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "mem/cache/cache.hh"

StreamPlacementManager::StreamPlacementManager(LLVMTraceCPU *_cpu,
                                               StreamEngine *_se)
    : cpu(_cpu), se(_se) {

  Cache *L2 = nullptr;
  Cache *L1D = nullptr;
  Cache *L1_5D = nullptr;
  for (auto so : this->cpu->getSimObjectList()) {
    // inform("so name %s.n", so->name().c_str());
    if (so->name() == "system.l2") {
      // L2 cache.
      L2 = dynamic_cast<Cache *>(so);
    } else if (so->name() == "system.cpu.dcache") {
      // L1 data cache.
      L1D = dynamic_cast<Cache *>(so);
    } else if (so->name() == "system.cpu.l1_5dcache") {
      // L1.5 data cache.
      L1_5D = dynamic_cast<Cache *>(so);
    }
  }

  assert(L1D != nullptr);
  this->caches.push_back(L1D);
  this->lookupLatency.push_back(L1D->getLookupLatency());

  if (L1_5D != nullptr) {
    // If we have this L1.5 data cache.
    this->caches.push_back(L1_5D);
    this->lookupLatency.push_back(L1_5D->getLookupLatency());
  }

  assert(L2 != nullptr);
  this->caches.push_back(L2);
  this->lookupLatency.push_back(L2->getLookupLatency());

  this->se->numCacheLevel = this->caches.size();
}

bool StreamPlacementManager::access(Stream *stream, Addr paddr, int packetSize,
                                    Stream::StreamMemAccess *memAccess) {

  auto coalescedStream = dynamic_cast<CoalescedStream *>(stream);
  if (coalescedStream == nullptr) {
    // So far we only consider coalesced streams.
    return false;
  }

  auto placeCacheLevel = this->whichCacheLevelToPlace(coalescedStream);

  this->se->numAccessPlacedInCacheLevel.sample(placeCacheLevel);
  auto footprint = coalescedStream->getFootprint(cpu->system->cacheLineSize());
  if (placeCacheLevel == 0) {
    this->se->numAccessFootprintL1.sample(footprint);
  } else if (placeCacheLevel == 1) {
    this->se->numAccessFootprintL2.sample(footprint);
  } else {
    this->se->numAccessFootprintL3.sample(footprint);
  }

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

  auto responseEvent = new ResponseEvent(cpu, memAccess, pkt);
  cpu->schedule(responseEvent, cpu->clockEdge(latency));
}

size_t
StreamPlacementManager::whichCacheLevelToPlace(CoalescedStream *stream) const {
  auto footprint = stream->getFootprint(cpu->system->cacheLineSize());
  for (size_t cacheLevel = 0; cacheLevel < this->caches.size(); ++cacheLevel) {
    auto cache = this->caches[cacheLevel];
    auto cacheCapacity = cache->getCacheSize() / cpu->system->cacheLineSize();
    // inform("Cache %d size %lu capacity %lu.\n", cacheLevel,
    //        cache->getCacheSize(), cacheCapacity);
    if (footprint < cacheCapacity) {
      return cacheLevel;
    }
  }
  assert(!this->caches.empty());
  return this->caches.size() - 1;
}