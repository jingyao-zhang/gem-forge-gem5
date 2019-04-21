#ifndef __CPU_TDG_ACCELERATOR_STREAM_ELEMENT_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_ELEMENT_HH__

#include "base/types.hh"
#include "cpu/llvm_trace/tdg_packet_handler.hh"

#include <unordered_set>

class Stream;

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

struct StreamElement : public TDGPacketHandler {
  std::unordered_set<StreamElement *> baseElements;
  StreamElement *next;
  Stream *stream;
  FIFOEntryIdx FIFOIdx;
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
  uint64_t cacheBlockAddrs[MAX_CACHE_BLOCKS];
  int cacheBlocks;

  std::unordered_set<PacketPtr> inflyLoadPackets;
  bool stored;

  StreamElement();

  void handlePacketResponse(LLVMTraceCPU *cpu, PacketPtr packet) override;

  void dump() const;

  void clear();
};

#endif