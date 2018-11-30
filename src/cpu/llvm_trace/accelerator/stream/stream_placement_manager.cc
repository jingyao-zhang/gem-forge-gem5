#include "stream_placement_manager.hh"

#include "coalesced_stream.hh"
#include "stream_engine.hh"

#include "base/output.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "mem/cache/cache.hh"
#include "mem/coherent_xbar.hh"

StreamPlacementManager::StreamPlacementManager(LLVMTraceCPU *_cpu,
                                               StreamEngine *_se)
    : cpu(_cpu), se(_se), L2Bus(nullptr), L2BusWidth(0) {

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
    } else if (so->name() == "system.tol2bus") {
      // L2 bus.
      this->L2Bus = dynamic_cast<CoherentXBar *>(so);
      const auto *XBarParam =
          dynamic_cast<const BaseXBarParams *>(so->params());
      this->L2BusWidth = XBarParam->width;
    }
  }

  assert(this->L2BusWidth != 0);
  assert(this->L2Bus != nullptr);

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

  if (!this->se->isPlacementEnabled()) {
    return false;
  }

  if (this->se->getPlacement() == "placement") {
    // Raw case.
    return false;
  }

  if (this->se->getPlacement() == "placement-no-mshr") {
    return this->accessNoMSHR(stream, paddr, packetSize, memAccess);
  }

  if (this->se->getPlacement() == "placement-expr") {
    return this->accessExpress(stream, paddr, packetSize, memAccess);
  }

  if (this->se->getPlacement() == "placement-expr-fp") {
    return this->accessExpressFootprint(stream, paddr, packetSize, memAccess);
  }

  auto coalescedStream = dynamic_cast<CoalescedStream *>(stream);
  if (coalescedStream == nullptr) {
    // So far we only consider coalesced streams.
    return false;
  }

  auto placeCacheLevel = this->whichCacheLevelToPlace(coalescedStream);

  this->se->numAccessPlacedInCacheLevel.sample(placeCacheLevel);

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

  if (hitLevel < placeCacheLevel) {
    this->se->numAccessHitHigherThanPlacedCacheLevel.sample(placeCacheLevel);
  } else if (hitLevel > placeCacheLevel) {
    this->se->numAccessHitLowerThanPlacedCacheLevel.sample(placeCacheLevel);
  }

  /**
   * 1. If hit above the place cache level, we schedule a response according
   *    to its tag lookup latency and issue a functional access to the place
   *    cache.
   * 2. Otherwise, we issue a real packet to the place cache.
   */
  auto pkt = this->createPacket(paddr, packetSize, memAccess);
  auto placeCache = this->caches[placeCacheLevel];
  if (this->se->isOraclePlacementEnabled()) {
    placeCache->recvAtomic(pkt);
    if (hitLevel == this->caches.size()) {
      latency += Cycles(10);
    }
    this->scheduleResponse(latency, memAccess, pkt);
    return true;
  }

  if (placeCacheLevel == 0) {
    // L1 cache is not handled by us.
    return false;
  }

  if (hitLevel <= placeCacheLevel) {
    placeCache->recvAtomic(pkt);
    this->scheduleResponse(latency, memAccess, pkt);
  } else {
    // Do a real cache access and allow.
    if (this->se->isOraclePlacementEnabled()) {
      auto sentLevel = hitLevel;
      if (sentLevel == this->caches.size()) {
        sentLevel -= 1;
      }
      auto sentCache = this->caches[sentLevel];
      this->sendTimingRequest(pkt, sentCache);
    } else {
      this->sendTimingRequest(pkt, placeCache);
    }
  }

  return true;
}

bool StreamPlacementManager::accessNoMSHR(Stream *stream, Addr paddr,
                                          int packetSize,
                                          Stream::StreamMemAccess *memAccess) {

  auto coalescedStream = dynamic_cast<CoalescedStream *>(stream);
  if (coalescedStream == nullptr) {
    // So far we only consider coalesced streams.
    return false;
  }

  // bool hasHit = false;
  // size_t hitLevel = this->caches.size();
  Cycles latency = Cycles(0);

  for (int cacheLevel = 0; cacheLevel < this->caches.size(); ++cacheLevel) {
    // latency += this->lookupLatency[cacheLevel];
    latency += Cycles(2);
    auto cache = this->caches[cacheLevel];
    auto isHitThisLevel = this->isHit(cache, paddr);
    if (isHitThisLevel) {
      // hasHit = true;
      // hitLevel = cacheLevel;
      break;
    }
  }

  /**
   * Schedule the response.
   */
  auto pkt = this->createPacket(paddr, packetSize, memAccess);
  this->caches[0]->recvAtomic(pkt);
  this->scheduleResponse(latency, memAccess, pkt);

  return true;
}

bool StreamPlacementManager::accessExpress(Stream *stream, Addr paddr,
                                           int packetSize,
                                           Stream::StreamMemAccess *memAccess) {
  int latency = 0;
  auto L1 = this->caches[0];
  auto &L1Stats = L1->getOrInitializeStreamStats(stream);
  if (L1Stats.accesses <= 100) {
    return false;
  }
  if (L1Stats.misses > L1Stats.accesses * 0.95f &&
      L1Stats.reuses < L1Stats.accesses * 0.1f) {

    latency++;

    // We decide to bypass L1.
    L1Stats.bypasses++;
    L1Stats.currentBypasses++;

    // Periodically clear the stats if we reaches a large number of bypasses.
    if (L1Stats.currentBypasses == 10000) {
      L1Stats.clear();
    }

    // Check if we want to bypass L2.
    auto L2 = this->caches[0];
    auto &L2Stats = L1->getOrInitializeStreamStats(stream);
    if (L2Stats.accesses > 100 && L2Stats.misses > L2Stats.accesses * 0.95f &&
        L2Stats.reuses < L2Stats.accesses * 0.1f) {
      // Bypassing L2.
      latency++;

      // Check if we want model the benefit of only transmitting a subblock.
      if (this->se->isPlacementBusEnabled()) {
        // We model the bus.
        if (this->se->getPlacementLat() == "sub") {
          // Packet size is not modified.
        } else {
          // Packet size is charged to 64.
          paddr = paddr & (~(64 - 1));
          packetSize = 64;
        }
        if (this->se->getPlacementLat() != "imm") {
          memAccess->setAdditionalDelay(latency);
        }
        auto pkt = this->createPacket(paddr, packetSize, memAccess);
        this->caches[0]->incHitCount(pkt);
        this->caches[1]->incHitCount(pkt);
        this->sendTimingRequestToL2Bus(pkt);
      } else {
        if (this->se->getPlacementLat() == "sub") {
          latency += divCeil(packetSize, this->L2BusWidth);
        } else {
          latency += 64 / this->L2BusWidth;
        }
        // Send request to L3.
        auto L3 = this->caches[2];
        if (this->se->getPlacementLat() != "imm") {
          memAccess->setAdditionalDelay(latency);
        }
        auto pkt = this->createPacket(paddr, packetSize, memAccess);
        this->caches[0]->incHitCount(pkt);
        this->caches[1]->incHitCount(pkt);
        this->sendTimingRequest(pkt, L3);
      }

      L2Stats.bypasses++;
      L2Stats.currentBypasses++;
      if (L2Stats.currentBypasses == 1000) {
        L2Stats.clear();
      }

      return true;
    } else {
      // Not bypassing L2.
      if (this->se->getPlacementLat() != "imm") {
        memAccess->setAdditionalDelay(latency);
      }
      auto pkt = this->createPacket(paddr, packetSize, memAccess);
      this->caches[0]->incHitCount(pkt);
      this->sendTimingRequest(pkt, L2);
      return true;
    }

    // this->scheduleResponse(latency, memAccess, pkt);
  }
  return false;
}

bool StreamPlacementManager::accessExpressFootprint(
    Stream *stream, Addr paddr, int packetSize,
    Stream::StreamMemAccess *memAccess) {

  // if (stream->getStreamName() == "(MEM train bb138 bb146::5(store))") {
  //   return false;
  // }

  // if (stream->getStreamName() == "(MEM train bb63 bb69::5(store))") {
  //   return false;
  // }

  if (this->se->isPlacementNoBypassingStore()) {
    if (stream->getStreamType() == "store") {
      return false;
    }
  }

  // Check the current cache level.
  auto cacheLevelIter = this->streamCacheLevelMap.find(stream);
  if (cacheLevelIter == this->streamCacheLevelMap.end()) {
    auto initCacheLevel = this->whichCacheLevelToPlace(stream);
    // initCacheLevel = 0;
    cacheLevelIter =
        this->streamCacheLevelMap.emplace(stream, initCacheLevel).first;
  }

  int latency = 0;
  auto bypassed = false;
  auto &cacheLevel = cacheLevelIter->second;

  // Try to fix them at L2.
  // if (stream->getStreamName() == "(MEM train bb25 bb33::tmp36(load))") {
  //   cacheLevel = 0;
  // }
  // if (stream->getStreamName() == "(MEM train bb158 bb160::tmp163(load))") {
  //   if (cacheLevel == 2) {
  //     panic("footprint %lu, true footprint \n",
  //           stream->getFootprint(cpu->system->cacheLineSize()));
  //   }
  // }

  // if (stream->getStreamName() == "(MEM train bb81 bb83::tmp90(load))") {
  //   cacheLevel = 1;
  // }

  if (cacheLevel > 0) {
    bypassed = true;
    // Bypassing L1.
    latency++;
    auto L1 = this->caches[0];
    auto &L1Stats = L1->getOrInitializeStreamStats(stream);
    L1Stats.bypasses++;
    L1Stats.currentBypasses++;

    auto L2 = this->caches[1];
    auto &L2Stats = L2->getOrInitializeStreamStats(stream);
    if (cacheLevel > 1) {
      // Bypassing L2.
      latency++;
      L2Stats.bypasses++;
      L2Stats.currentBypasses++;

      // Check if we want model the benefit of only transmitting a subblock.
      if (this->se->isPlacementBusEnabled()) {
        // We model the bus.
        if (this->se->getPlacementLat() == "sub") {
          // Packet size is not modified.
        } else {
          // Packet size is charged to 64.
          paddr = paddr & (~(64 - 1));
          packetSize = 64;
        }
        if (this->se->getPlacementLat() != "imm") {
          memAccess->setAdditionalDelay(latency);
        }
        auto pkt = this->createPacket(paddr, packetSize, memAccess);
        this->caches[0]->incHitCount(pkt);
        this->caches[1]->incHitCount(pkt);
        this->sendTimingRequestToL2Bus(pkt);
      } else {
        if (this->se->getPlacementLat() == "sub") {
          latency += divCeil(packetSize, this->L2BusWidth);
        } else {
          latency += 64 / this->L2BusWidth;
        }

        auto L3 = this->caches[2];
        if (this->se->getPlacementLat() != "imm") {
          memAccess->setAdditionalDelay(latency);
        }
        auto pkt = this->createPacket(paddr, packetSize, memAccess);
        this->caches[0]->incHitCount(pkt);
        this->caches[1]->incHitCount(pkt);
        this->sendTimingRequest(pkt, L3);
      }
    } else {
      // Do not bypassing L2.
      if (this->se->getPlacementLat() != "imm") {
        memAccess->setAdditionalDelay(latency);
      }
      auto pkt = this->createPacket(paddr, packetSize, memAccess);
      this->caches[0]->incHitCount(pkt);
      this->sendTimingRequest(pkt, L2);
    }
  }
  // Adjust our cache level based on the statistics.
  auto placedCache = this->caches[cacheLevel];
  auto &placedCacheStats = placedCache->getOrInitializeStreamStats(stream);

  // Check if we have enough access to decide which level of cache.
  if (placedCacheStats.currentAccesses > 100 &&
      placedCacheStats.currentMisses >
          placedCacheStats.currentAccesses * 0.95f &&
      placedCacheStats.currentReuses <
          placedCacheStats.currentAccesses * 0.1f) {
    // There are still a lot of miss and little reuse at this level of cache,
    // we should try to lower the cache level.
    if (cacheLevel < 2) {
      cacheLevel++;
      placedCacheStats.clear();
    }
  } else if (placedCacheStats.currentAccesses > 1000 &&
             placedCacheStats.currentReuses >
                 placedCacheStats.currentAccesses * 0.8f) {
    // There is a lot of reuse here, try to promote the stream to higher
    // level.
    if (cacheLevel > 0) {
      cacheLevel--;
      placedCacheStats.clear();
    }
  } else {
    if (this->se->isPlacementPeriodReset()) {
      auto &L1Stats = this->caches[0]->getOrInitializeStreamStats(stream);
      if (L1Stats.currentBypasses == 10000) {
        L1Stats.clear();
        cacheLevel = 0;
        placedCacheStats.clear();
      }
    }
  }

  return bypassed;
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

void StreamPlacementManager::sendTimingRequestToL2Bus(PacketPtr pkt) {
  assert(this->L2Bus != nullptr);
  auto slavePort = this->L2Bus->getSlavePort(3);
  auto streamAwareSlavePort =
      dynamic_cast<CoherentXBar::StreamAwareCoherentXBarSlavePort *>(slavePort);
  if (streamAwareSlavePort == nullptr) {
    panic("Failed to get the stream aware slave port from L2 bus.");
  }
  streamAwareSlavePort->recvTimingReqForStream(pkt);
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

size_t StreamPlacementManager::whichCacheLevelToPlace(Stream *stream) const {
  auto footprint = stream->getFootprint(cpu->system->cacheLineSize());
  if (this->se->getPlacement() == "placement-footprint") {
    footprint = stream->getTrueFootprint();
  }
  auto placeCacheLevel = this->caches.size() - 1;
  for (size_t cacheLevel = 0; cacheLevel < this->caches.size(); ++cacheLevel) {
    auto cache = this->caches[cacheLevel];
    auto cacheCapacity = cache->getCacheSize() / cpu->system->cacheLineSize();
    // inform("Cache %d size %lu capacity %lu.\n", cacheLevel,
    //        cache->getCacheSize(), cacheCapacity);
    if (footprint < cacheCapacity) {
      placeCacheLevel = cacheLevel;
      break;
    }
  }
  if (placeCacheLevel == 0) {
    this->se->numAccessFootprintL1.sample(footprint);
  } else if (placeCacheLevel == 1) {
    this->se->numAccessFootprintL2.sample(footprint);
  } else {
    this->se->numAccessFootprintL3.sample(footprint);
  }
  assert(!this->caches.empty());
  return placeCacheLevel;
}

void StreamPlacementManager::dumpStreamCacheStats() {
  {
    auto L1 = this->caches[0];
    auto &o = *simout.findOrCreate("L1Stream.txt")->stream();
    L1->dumpStreamStats(o);
  }
  {
    auto L2 = this->caches[1];
    auto &o = *simout.findOrCreate("L2Stream.txt")->stream();
    L2->dumpStreamStats(o);
  }
  {
    auto L3 = this->caches[2];
    auto &o = *simout.findOrCreate("L3Stream.txt")->stream();
    L3->dumpStreamStats(o);
  }
}
