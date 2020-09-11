#ifndef __CPU_TDG_ACCELERATOR_STREAM_PLACEMENT_MANAGER_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_PLACEMENT_MANAGER_HH__

#include "stream.hh"

class Cache;
class CoherentXBar;
class CoalescedStream;

class StreamPlacementManager {
public:
  StreamPlacementManager(StreamEngine *_se);

  bool access(CacheBlockBreakdownAccess &cacheBlockBreakdown,
              StreamElement *element, bool isWrite = false);

  void dumpCacheStreamAwarePortStatus();

  void dumpStreamCacheStats();

  struct ResponseEvent : public Event {
  public:
    GemForgeCPUDelegator *cpuDelegator;
    StreamMemAccess *memAccess;
    PacketPtr pkt;
    std::string n;
    ResponseEvent(GemForgeCPUDelegator *_cpuDelegator,
                  StreamMemAccess *_memAccess, PacketPtr _pkt)
        : cpuDelegator(_cpuDelegator), memAccess(_memAccess), pkt(_pkt),
          n("StreamPlacementResponseEvent") {}
    void process() override {
      this->memAccess->handlePacketResponse(this->cpuDelegator, this->pkt);
    }

    const char *description() const { return "StreamPlacementResponseEvent"; }

    const std::string name() const { return this->n; }
  };

private:
  StreamEngine *se;

  std::unordered_map<Stream *, int> streamCacheLevelMap;

  std::vector<Cache *> caches;
  std::vector<Cycles> lookupLatency;
  CoherentXBar *L2Bus;

  uint32_t L2BusWidth;

  bool accessNoMSHR(Stream *stream,
                    CacheBlockBreakdownAccess &cacheBlockBreakdown,
                    StreamElement *element, bool isWrite);

  bool accessExpress(Stream *stream,
                     CacheBlockBreakdownAccess &cacheBlockBreakdown,
                     StreamElement *element, bool isWrite);

  bool accessExpressFootprint(Stream *stream,
                              CacheBlockBreakdownAccess &cacheBlockBreakdown,
                              StreamElement *element, bool isWrite);

  size_t whichCacheLevelToPlace(Stream *stream) const;
  int getPlacedCacheLevelByFootprint(Stream *stream) const;
  int getOrInitializePlacedCacheLevel(Stream *stream);
  void updatePlacedCacheLevel(Stream *stream);

  PacketPtr createPacket(Addr paddr, int size, StreamElement *element,
                         CacheBlockBreakdownAccess &cacheBlockBreakdown,
                         bool isWrite) const;

  bool isHit(Cache *cache, Addr paddr) const;
  void scheduleResponse(Cycles latency, StreamElement *element, PacketPtr pkt);
  void sendTimingRequest(PacketPtr pkt, Cache *cache);
  void sendTimingRequestToL2Bus(PacketPtr pkt);
};

#endif
