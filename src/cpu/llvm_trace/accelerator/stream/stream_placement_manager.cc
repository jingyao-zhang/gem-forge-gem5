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
  size_t hitLevel = this->caches.size();
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
    if (!hasHit && (isHitThisLevel)) {
      hasHit = true;
      hitLevel = cacheLevel;
    }
  }

  /**
   * 1. If hit above the place cache level, we schedule a response according
   *    to its tag lookup latency and issue a functional access to the place
   *    cache.
   * 2. Otherwise, we issue a real packet to the place cache.
   */
  auto pkt = this->createPacket(paddr, packetSize, memAccess);
  auto placeCache = this->caches[placeCacheLevel];
  if (hitLevel < placeCacheLevel) {
    placeCache->functionalAccess(pkt, true);
    this->scheduleResponse(latency, memAccess, pkt);
  } else {
    // Do a real cache access and allow.
    this->sendTimingRequest(pkt, placeCache);
  }

  return true;
}

bool StreamPlacementManager::isHit(Cache *cache, Addr paddr) const {
  return cache->inCache(paddr, false);
}

PacketPtr
StreamPlacementManager::createPacket(Addr paddr, int size,
                                     Stream::StreamMemAccess *memAccess) const {
  RequestPtr req =
      new Request(paddr, size, 0, Request::funcMasterId,
                  reinterpret_cast<InstSeqNum>(memAccess), 0 /*Context id*/);
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

void StreamPlacementManager::sendTimingRequest(PacketPtr pkt, Cache *cache) {

  auto cpuSidePort = &(cache->getSlavePort("cpu_side", 0));
  auto streamAwareCpuSidePort =
      dynamic_cast<Cache::StreamAwareCpuSidePort *>(cpuSidePort);
  if (streamAwareCpuSidePort == nullptr) {
    panic("Failed to get the stream aware cache cpu side port.");
  }

  streamAwareCpuSidePort->recvTimingReqForStream(pkt);
}

void StreamPlacementManager::dumpCacheStreamAwarePortStatus() {
  for (auto cache : this->caches) {
    auto cpuSidePort = &(cache->getSlavePort("cpu_side", 0));
    auto streamAwareCpuSidePort =
        dynamic_cast<Cache::StreamAwareCpuSidePort *>(cpuSidePort);
    if (streamAwareCpuSidePort != nullptr) {
      inform("%s StreamAwareCpuSidePort Blocked ? %d Pkts %lu  =========",
             cache->name().c_str(), streamAwareCpuSidePort->isBlocked(),
             streamAwareCpuSidePort->blockedPkts.size());
      // for (auto pkt : streamAwareCpuSidePort->blockedPkts) {
      //   inform("blocked stream pkt: %p", pkt);
      // }
    }
  }
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
