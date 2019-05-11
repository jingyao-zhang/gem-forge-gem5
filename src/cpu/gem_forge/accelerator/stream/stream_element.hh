#ifndef __CPU_TDG_ACCELERATOR_STREAM_ELEMENT_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_ELEMENT_HH__

#include "base/types.hh"
#include "cpu/gem_forge/tdg_packet_handler.hh"

#include <unordered_map>
#include <unordered_set>

class Stream;
class StreamEngine;
class StreamStoreInst;

struct FIFOEntryIdx {
  uint64_t streamInstance;
  uint64_t configSeqNum;
  uint64_t entryIdx;
  FIFOEntryIdx();
  FIFOEntryIdx(uint64_t _streamInstance, uint64_t _configSeqNum,
               uint64_t _entryIdx);
  void next() { this->entryIdx++; }
  void newInstance(uint64_t configSeqNum) {
    this->entryIdx = 0;
    this->streamInstance++;
    this->configSeqNum = configSeqNum;
  }

  bool operator==(const FIFOEntryIdx &other) const {
    return this->streamInstance == other.streamInstance &&
           this->entryIdx == other.entryIdx;
  }
  bool operator!=(const FIFOEntryIdx &other) const {
    return !(this->operator==(other));
  }
};

/**
 * Represent the breakdown of one element according to cache block size.
 */
struct CacheBlockBreakdownAccess {
  // Which cache block this access belongs to.
  uint64_t cacheBlockVirtualAddr;
  // The actual virtual address.
  uint64_t virtualAddr;
  // The actual size.
  uint8_t size;
};

class StreamElement;
/**
 * This is used as a handler to the response packet.
 * The stream aware cache also use this to find the stream the packet belongs
 * to.
 */
class StreamMemAccess final : public TDGPacketHandler {
public:
  StreamMemAccess(Stream *_stream, StreamElement *_element,
                  Addr _cacheBlockVirtualAddr, int _additionalDelay = 0)
      : stream(_stream), element(_element),
        cacheBlockVirtualAddr(_cacheBlockVirtualAddr),
        additionalDelay(_additionalDelay) {}
  virtual ~StreamMemAccess() {}
  void handlePacketResponse(LLVMTraceCPU *cpu, PacketPtr packet) override;
  void handlePacketResponse(PacketPtr packet);
  // This cache block is fetched in by some other StreamMemAccess.
  void handleStreamEngineResponse();

  Stream *getStream() const { return this->stream; }

  void setAdditionalDelay(int additionalDelay) {
    this->additionalDelay = additionalDelay;
  }

  struct ResponseEvent : public Event {
  public:
    LLVMTraceCPU *cpu;
    StreamMemAccess *memAccess;
    PacketPtr pkt;
    std::string n;
    ResponseEvent(LLVMTraceCPU *_cpu, StreamMemAccess *_memAccess,
                  PacketPtr _pkt)
        : cpu(_cpu), memAccess(_memAccess), pkt(_pkt),
          n("StreamMemAccessResponseEvent") {}
    void process() override {
      this->memAccess->handlePacketResponse(this->cpu, this->pkt);
    }

    const char *description() const { return "StreamMemAccessResponseEvent"; }

    const std::string name() const { return this->n; }
  };

  Stream *const stream;
  StreamElement *const element;
  Addr cacheBlockVirtualAddr;
  /**
   * Additional delay we want to add after we get the response.
   */
  int additionalDelay;
};

struct StreamElement {
  std::unordered_set<StreamElement *> baseElements;
  StreamElement *next;
  Stream *stream;
  StreamEngine *se;
  FIFOEntryIdx FIFOIdx;
  /**
   * Whether the first user of this stream element has been dispatched.
   * This is used to determine the first user the of the stream element
   * and allocate entry in the load queue.
   */
  uint64_t firstUserSeqNum;
  bool isAddrReady;
  bool isValueReady;

  Cycles allocateCycle;
  Cycles valueReadyCycle;
  Cycles firstCheckCycle;

  /**
   * Small vector stores the cache blocks this element touched.
   */
  uint64_t addr;
  uint64_t size;
  static constexpr int MAX_CACHE_BLOCKS = 10;
  CacheBlockBreakdownAccess cacheBlockBreakdownAccesses[MAX_CACHE_BLOCKS];
  int cacheBlocks;

  // Store the infly mem accesses for this element, basically for load.
  std::unordered_set<StreamMemAccess *> inflyMemAccess;
  // Store the infly writeback memory accesses.
  std::unordered_map<StreamStoreInst *, std::unordered_set<StreamMemAccess *>>
      inflyWritebackMemAccess;
  // Store all the allocated mem accesses. May be different to inflyMemAccess
  // if some the element is released before the result comes back, e.g. unused
  // element.
  std::unordered_set<StreamMemAccess *> allocatedMemAccess;

  bool stored;

  StreamElement(StreamEngine *_se);

  StreamMemAccess *
  allocateStreamMemAccess(const CacheBlockBreakdownAccess &cacheBlockBreakDown);
  void handlePacketResponse(StreamMemAccess *memAccess);
  void markValueReady();

  void dump() const;

  void clear();
  Stream *getStream() {
    assert(this->stream != nullptr && "Null stream in the element.");
    return this->stream;
  }
};

#endif