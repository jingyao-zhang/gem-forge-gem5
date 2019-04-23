#ifndef __CPU_TDG_ACCELERATOR_STREAM_PLACEMENT_MANAGER_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_PLACEMENT_MANAGER_HH__

#include "stream.hh"

class Cache;
class CoherentXBar;
class CoalescedStream;

class StreamPlacementManager {
public:
  StreamPlacementManager(LLVMTraceCPU *_cpu, StreamEngine *_se);

  bool access(Addr paddr, int packetSize, StreamElement *element);

  void dumpCacheStreamAwarePortStatus();

  void dumpStreamCacheStats();

  struct ResponseEvent : public Event {
  public:
    LLVMTraceCPU *cpu;
    StreamMemAccess *memAccess;
    PacketPtr pkt;
    std::string n;
    ResponseEvent(LLVMTraceCPU *_cpu, StreamMemAccess *_memAccess,
                  PacketPtr _pkt)
        : cpu(_cpu), memAccess(_memAccess), pkt(_pkt),
          n("StreamPlacementResponseEvent") {}
    void process() override {
      this->memAccess->handlePacketResponse(this->cpu, this->pkt);
    }

    const char *description() const { return "StreamPlacementResponseEvent"; }

    const std::string name() const { return this->n; }
  };

private:
  LLVMTraceCPU *cpu;
  StreamEngine *se;

  std::unordered_map<Stream *, int> streamCacheLevelMap;

  std::vector<Cache *> caches;
  std::vector<Cycles> lookupLatency;
  CoherentXBar *L2Bus;

  uint32_t L2BusWidth;

  bool accessNoMSHR(Stream *stream, Addr paddr, int packetSize,
                    StreamElement *element);

  bool accessExpress(Stream *stream, Addr paddr, int packetSize,
                     StreamElement *element);

  bool accessExpressFootprint(Stream *stream, Addr paddr, int packetSize,
                              StreamElement *element);

  size_t whichCacheLevelToPlace(Stream *stream) const;

  PacketPtr createPacket(Addr paddr, int size, StreamElement *element) const;

  bool isHit(Cache *cache, Addr paddr) const;
  void scheduleResponse(Cycles latency, StreamElement *element, PacketPtr pkt);
  void sendTimingRequest(PacketPtr pkt, Cache *cache);
  void sendTimingRequestToL2Bus(PacketPtr pkt);
};

#endif
