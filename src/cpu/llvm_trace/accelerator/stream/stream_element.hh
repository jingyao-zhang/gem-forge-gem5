#ifndef __CPU_TDG_ACCELERATOR_STREAM_ELEMENT_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_ELEMENT_HH__

#include "base/types.hh"
#include "cpu/llvm_trace/tdg_packet_handler.hh"

#include <unordered_set>

class Stream;

struct StreamElement : public TDGPacketHandler {
  std::unordered_set<StreamElement *> baseElements;
  StreamElement *next;
  Stream *stream;
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
  static constexpr int MAX_CACHE_BLOCKS = 4;
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