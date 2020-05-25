#include "stream_placement_manager.hh"

#include "coalesced_stream.hh"
#include "stream_engine.hh"

#include "base/output.hh"
#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"
#include "mem/cache/cache.hh"
#include "mem/coherent_xbar.hh"

StreamPlacementManager::StreamPlacementManager(
    GemForgeCPUDelegator *_cpuDelegator, StreamEngine *_se)
    : cpuDelegator(_cpuDelegator), se(_se), L2Bus(nullptr), L2BusWidth(0) {

  Cache *L2 = nullptr;
  Cache *L1D = nullptr;
  Cache *L1_5D = nullptr;

  for (auto so : this->se->getSimObjectList()) {
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

  assert(this->caches.size() == 3 &&
         "So far we only support 3 level of cache for stream placement.");
}

bool StreamPlacementManager::access(
    CacheBlockBreakdownAccess &cacheBlockBreakdown,
    StreamElement *element, bool isWrite) {

  if (!this->se->isPlacementEnabled()) {
    return false;
  }

  if (this->se->getPlacement() == "placement") {
    // Raw case.
    return false;
  }

  auto stream = element->getStream();
  assert(stream != nullptr && "Missing stream in StreamElement.");

  if (this->se->getPlacement() == "placement-no-mshr") {
    return this->accessNoMSHR(stream, cacheBlockBreakdown, element, isWrite);
  }

  if (this->se->getPlacement() == "placement-expr") {
    return this->accessExpress(stream, cacheBlockBreakdown, element, isWrite);
  }

  if (this->se->getPlacement() == "placement-expr-fp") {
    return this->accessExpressFootprint(stream, cacheBlockBreakdown, element,
                                        isWrite);
  }
  return false;
}

bool StreamPlacementManager::accessNoMSHR(
    Stream *stream, CacheBlockBreakdownAccess &cacheBlockBreakdown,
    StreamElement *element, bool isWrite) {

  auto coalescedStream = dynamic_cast<CoalescedStream *>(stream);
  if (coalescedStream == nullptr) {
    // So far we only consider coalesced streams.
    return false;
  }

  // bool hasHit = false;
  // size_t hitLevel = this->caches.size();
  Cycles latency = Cycles(0);

  auto vaddr = cacheBlockBreakdown.virtualAddr;
  auto packetSize = cacheBlockBreakdown.size;
  Addr paddr;
  if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
    panic("Failed translate vaddr %#x.\n", vaddr);
  }

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
  auto pkt = this->createPacket(paddr, packetSize, element, cacheBlockBreakdown,
                                isWrite);
  this->caches[0]->recvAtomic(pkt);
  this->scheduleResponse(latency, element, pkt);

  return true;
}

bool StreamPlacementManager::accessExpress(
    Stream *stream, CacheBlockBreakdownAccess &cacheBlockBreakdown,
    StreamElement *element, bool isWrite) {

  // Do not bypass for the first 100 accesses.
  auto L1 = this->caches[0];
  auto &L1Stats = L1->getOrInitializeStreamStats(stream);
  if (L1Stats.accesses <= 100) {
    return false;
  }

  auto vaddr = cacheBlockBreakdown.virtualAddr;
  auto packetSize = cacheBlockBreakdown.size;
  Addr paddr;
  if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
    panic("Failed translate vaddr %#x.\n", vaddr);
  }

  int latency = 0;
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
    auto L2 = this->caches[1];
    auto &L2Stats = L2->getOrInitializeStreamStats(stream);
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
        auto pkt = this->createPacket(paddr, packetSize, element,
                                      cacheBlockBreakdown, isWrite);
        if (this->se->getPlacementLat() != "imm") {
          auto memAccess =
              reinterpret_cast<StreamMemAccess *>(pkt->req->getReqInstSeqNum());
          memAccess->setAdditionalDelay(latency);
        }
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
        auto pkt = this->createPacket(paddr, packetSize, element,
                                      cacheBlockBreakdown, isWrite);
        if (this->se->getPlacementLat() != "imm") {
          auto memAccess =
              reinterpret_cast<StreamMemAccess *>(pkt->req->getReqInstSeqNum());
          memAccess->setAdditionalDelay(latency);
        }
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
      auto pkt = this->createPacket(paddr, packetSize, element,
                                    cacheBlockBreakdown, isWrite);
      if (this->se->getPlacementLat() != "imm") {
        auto memAccess =
            reinterpret_cast<StreamMemAccess *>(pkt->req->getReqInstSeqNum());
        memAccess->setAdditionalDelay(latency);
      }
      this->caches[0]->incHitCount(pkt);
      this->sendTimingRequest(pkt, L2);
      return true;
    }

    // this->scheduleResponse(latency, memAccess, pkt);
  }

  /******************************************************
   * No Bypassing
   ******************************************************/
  return false;
}

bool StreamPlacementManager::accessExpressFootprint(
    Stream *stream, CacheBlockBreakdownAccess &cacheBlockBreakdown,
    StreamElement *element, bool isWrite) {

  if (this->se->isPlacementNoBypassingStore()) {
    if (stream->getStreamType() == ::LLVM::TDG::StreamInfo_Type_ST) {
      return false;
    }
  }

  auto vaddr = cacheBlockBreakdown.virtualAddr;
  auto packetSize = cacheBlockBreakdown.size;
  Addr paddr;
  if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
    panic("Failed translate vaddr %#x.\n", vaddr);
  }

  int latency = 0;
  auto bypassed = false;
  auto placedCacheLevel = this->getOrInitializePlacedCacheLevel(stream);
  this->se->numAccessPlacedInCacheLevel[placedCacheLevel]++;
  // Get the hit level.
  auto hitCacheLevel = this->caches.size();
  for (int c = 0; c < this->caches.size(); ++c) {
    if (this->isHit(this->caches[c], paddr)) {
      hitCacheLevel = c;
      break;
    }
  }
  if (hitCacheLevel > placedCacheLevel) {
    // Higher means closer to cpu.
    this->se->numAccessHitLowerThanPlacedCacheLevel[placedCacheLevel]++;
  } else if (hitCacheLevel < placedCacheLevel) {
    this->se->numAccessHitHigherThanPlacedCacheLevel[placedCacheLevel]++;
  }

  if (placedCacheLevel > 0) {
    bypassed = true;
    // Bypassing L1.
    latency++;
    auto L1 = this->caches[0];
    auto &L1Stats = L1->getOrInitializeStreamStats(stream);
    L1Stats.bypasses++;
    L1Stats.currentBypasses++;

    auto L2 = this->caches[1];
    auto &L2Stats = L2->getOrInitializeStreamStats(stream);
    if (placedCacheLevel > 1) {
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
        auto pkt = this->createPacket(paddr, packetSize, element,
                                      cacheBlockBreakdown, isWrite);
        if (this->se->getPlacementLat() != "imm") {
          auto memAccess =
              reinterpret_cast<StreamMemAccess *>(pkt->req->getReqInstSeqNum());
          memAccess->setAdditionalDelay(latency);
        }
        // this->caches[0]->incHitCount(pkt);
        // this->caches[1]->incHitCount(pkt);
        this->sendTimingRequestToL2Bus(pkt);
      } else {
        if (this->se->getPlacementLat() == "sub") {
          latency += divCeil(packetSize, this->L2BusWidth);
        } else {
          latency += 64 / this->L2BusWidth;
        }

        auto L3 = this->caches[2];
        auto pkt = this->createPacket(paddr, packetSize, element,
                                      cacheBlockBreakdown, isWrite);
        if (this->se->getPlacementLat() != "imm") {
          auto memAccess =
              reinterpret_cast<StreamMemAccess *>(pkt->req->getReqInstSeqNum());
          memAccess->setAdditionalDelay(latency);
        }
        // this->caches[0]->incHitCount(pkt);
        // this->caches[1]->incHitCount(pkt);
        this->sendTimingRequest(pkt, L3);
      }
    } else {
      // Do not bypassing L2.
      auto pkt = this->createPacket(paddr, packetSize, element,
                                    cacheBlockBreakdown, isWrite);
      if (this->se->getPlacementLat() != "imm") {
        auto memAccess =
            reinterpret_cast<StreamMemAccess *>(pkt->req->getReqInstSeqNum());
        memAccess->setAdditionalDelay(latency);
      }
      // this->caches[0]->incHitCount(pkt);
      this->sendTimingRequest(pkt, L2);
    }
  }
  // Adjust our cache level based on the statistics.
  // this->updatePlacedCacheLevel(stream);

  return bypassed;
}

bool StreamPlacementManager::isHit(Cache *cache, Addr paddr) const {
  return cache->inCache(paddr, false);
}

PacketPtr StreamPlacementManager::createPacket(
    Addr paddr, int size, StreamElement *element,
    CacheBlockBreakdownAccess &cacheBlockBreakdown, bool isWrite) const {
  auto memAccess = element->allocateStreamMemAccess(cacheBlockBreakdown);
  uint8_t *data = nullptr;
  if (isWrite) {
    data = new uint8_t[size];
  }
  auto pkt = GemForgePacketHandler::createGemForgePacket(
      paddr, size, memAccess, data, Request::funcMasterId, 0, 0);
  if (isWrite) {
    delete[] data;
  }
  /**
   * Remember to add this to the element infly memAccess set.
   */
  cacheBlockBreakdown.memAccess = memAccess;
  return pkt;
}

void StreamPlacementManager::scheduleResponse(Cycles latency,
                                              StreamElement *element,
                                              PacketPtr pkt) {
  auto responseEvent = new ResponseEvent(
      cpuDelegator,
      reinterpret_cast<StreamMemAccess *>(pkt->req->getReqInstSeqNum()), pkt);
  cpuDelegator->schedule(responseEvent, latency);
}

void StreamPlacementManager::sendTimingRequest(PacketPtr pkt, Cache *cache) {

  auto cpuSidePort = &(cache->getPort("cpu_side", 0));
  auto streamAwareCpuSidePort =
      dynamic_cast<Cache::StreamAwareCpuSidePort *>(cpuSidePort);
  if (streamAwareCpuSidePort == nullptr) {
    panic("Failed to get the stream aware cache cpu side port.");
  }

  streamAwareCpuSidePort->recvTimingReqForStream(pkt);
}

void StreamPlacementManager::sendTimingRequestToL2Bus(PacketPtr pkt) {
  assert(this->L2Bus != nullptr);
  auto slavePort = &(this->L2Bus->getPort("slave", 3));
  auto streamAwareSlavePort =
      dynamic_cast<CoherentXBar::StreamAwareCoherentXBarSlavePort *>(slavePort);
  if (streamAwareSlavePort == nullptr) {
    panic("Failed to get the stream aware slave port from L2 bus.");
  }
  streamAwareSlavePort->recvTimingReqForStream(pkt);
}

void StreamPlacementManager::dumpCacheStreamAwarePortStatus() {
  for (auto cache : this->caches) {
    auto cpuSidePort = &(cache->getPort("cpu_side", 0));
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

int StreamPlacementManager::getPlacedCacheLevelByFootprint(
    Stream *stream) const {
  auto footprint = stream->getFootprint(cpuDelegator->cacheLineSize());
  if (this->se->getPlacement() == "placement-footprint") {
    footprint = stream->getTrueFootprint();
  }
  auto placeCacheLevel = this->caches.size() - 1;
  for (size_t cacheLevel = 0; cacheLevel < this->caches.size(); ++cacheLevel) {
    auto cache = this->caches[cacheLevel];
    auto cacheCapacity = cache->getCacheSize() / cpuDelegator->cacheLineSize();
    if (footprint < cacheCapacity) {
      placeCacheLevel = cacheLevel;
      break;
    }
  }
  return placeCacheLevel;
}

int StreamPlacementManager::getOrInitializePlacedCacheLevel(Stream *stream) {
  auto cacheLevelIter = this->streamCacheLevelMap.find(stream);
  if (cacheLevelIter == this->streamCacheLevelMap.end()) {
    // Initialize the placed cache level by footprint.
    auto initCacheLevel = this->getPlacedCacheLevelByFootprint(stream);
    cacheLevelIter =
        this->streamCacheLevelMap.emplace(stream, initCacheLevel).first;
  }
  return cacheLevelIter->second;
}

void StreamPlacementManager::updatePlacedCacheLevel(Stream *stream) {
  // Adjust our cache level based on the statistics.
  auto &placedCacheLevel = this->streamCacheLevelMap.at(stream);
  auto placedCache = this->caches[placedCacheLevel];
  auto &placedCacheStats = placedCache->getOrInitializeStreamStats(stream);

  // Check if we have enough access to decide which level of cache.
  if (placedCacheStats.currentAccesses > 100 &&
      placedCacheStats.currentMisses >
          placedCacheStats.currentAccesses * 0.95f &&
      placedCacheStats.currentReuses <
          placedCacheStats.currentAccesses * 0.1f) {
    // There are still a lot of miss and little reuse at this level of cache,
    // we should try to lower the cache level.
    if (placedCacheLevel < 2) {
      placedCacheLevel++;
      placedCacheStats.clear();
    }
  } else if (placedCacheStats.currentAccesses > 1000 &&
             placedCacheStats.currentReuses >
                 placedCacheStats.currentAccesses * 0.8f) {
    // There is a lot of reuse here, try to promote the stream to higher
    // level.
    if (placedCacheLevel > 0) {
      placedCacheLevel--;
      placedCacheStats.clear();
    }
  } else {
    if (this->se->isPlacementPeriodReset()) {
      auto &L1Stats = this->caches[0]->getOrInitializeStreamStats(stream);
      if (L1Stats.currentBypasses == 10000) {
        L1Stats.clear();
        placedCacheLevel = 0;
        placedCacheStats.clear();
      }
    }
  }
}

size_t StreamPlacementManager::whichCacheLevelToPlace(Stream *stream) const {
  auto footprint = stream->getFootprint(cpuDelegator->cacheLineSize());
  if (this->se->getPlacement() == "placement-footprint") {
    footprint = stream->getTrueFootprint();
  }
  auto placeCacheLevel = this->caches.size() - 1;
  for (size_t cacheLevel = 0; cacheLevel < this->caches.size(); ++cacheLevel) {
    auto cache = this->caches[cacheLevel];
    auto cacheCapacity = cache->getCacheSize() / cpuDelegator->cacheLineSize();
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
  for (int cacheLevel = 0; cacheLevel < this->caches.size(); ++cacheLevel) {
    auto cache = this->caches[cacheLevel];
    auto &o =
        *simout.findOrCreate("StreamCache." + cache->name() + ".txt")->stream();
    cache->dumpStreamStats(o);
  }
}
