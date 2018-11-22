#ifndef __CPU_TDG_ACCELERATOR_STREAM_PLACEMENT_MANAGER_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_PLACEMENT_MANAGER_HH__

#include "stream.hh"

class Cache;
class CoalescedStream;

class StreamPlacementManager {
public:
  StreamPlacementManager(LLVMTraceCPU *_cpu, StreamEngine *_se);

  bool access(Stream *stream, Addr paddr, int packetSize,
              Stream::StreamMemAccess *memAccess);

  struct ResponseEvent : public Event {
  public:
    LLVMTraceCPU *cpu;
    Stream::StreamMemAccess *memAccess;
    PacketPtr pkt;
    std::string n;
    ResponseEvent(LLVMTraceCPU *_cpu, Stream::StreamMemAccess *_memAccess,
                  PacketPtr _pkt)
        : cpu(_cpu), memAccess(_memAccess), pkt(_pkt),
          n("StreamPlacementResponseEvent") {}
    void process() override {
      this->memAccess->handlePacketResponse(this->cpu, this->pkt);
    }

    const char* description() const {
      return "StreamPlacementResponseEvent";
    }

    const std::string name() const { return this->n; }
  };

private:
  LLVMTraceCPU *cpu;
  StreamEngine *se;

  std::vector<Cache *> caches;
  std::vector<Cycles> lookupLatency;

  size_t whichCacheLevelToPlace(CoalescedStream *stream) const;

  PacketPtr createPacket(Addr paddr, int size) const;

  bool isHit(Cache *cache, Addr paddr) const;
  void scheduleResponse(Cycles latency, Stream::StreamMemAccess *memAccess,
                        PacketPtr pkt);
};

#endif