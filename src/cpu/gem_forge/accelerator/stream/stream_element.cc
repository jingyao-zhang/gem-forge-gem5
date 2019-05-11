#include "stream_element.hh"
#include "stream.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "debug/StreamEngine.hh"

#define STREAM_DPRINTF(stream, format, args...)                                \
  DPRINTF(StreamEngine, "[%s]: " format, stream->getStreamName().c_str(),      \
          ##args)

#define STREAM_PANIC(stream, format, args...)                                  \
  panic("[%s]: " format, stream->getStreamName().c_str(), ##args)

#define STREAM_ELEMENT_DPRINTF(element, format, args...)                       \
  STREAM_DPRINTF(element->getStream(), "[%lu, %lu]: " format,                  \
                 element->FIFOIdx.streamInstance, element->FIFOIdx.entryIdx,   \
                 ##args)

#define STREAM_ELEMENT_PANIC(element, format, args...)                         \
  element->se->dump();                                                         \
  STREAM_PANIC(element->getStream(), "[%lu, %lu]: " format,                    \
               element->FIFOIdx.streamInstance, element->FIFOIdx.entryIdx,     \
               ##args)

FIFOEntryIdx::FIFOEntryIdx()
    : streamInstance(0), configSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
      entryIdx(0) {}

FIFOEntryIdx::FIFOEntryIdx(uint64_t _streamInstance, uint64_t _configSeqNum,
                           uint64_t _entryIdx)
    : streamInstance(_streamInstance), configSeqNum(_configSeqNum),
      entryIdx(_entryIdx) {}

void StreamMemAccess::handlePacketResponse(PacketPtr packet) {
  // API for stream-aware cache, as it doesn't have the cpu.
  this->handlePacketResponse(this->getStream()->getCPU(), packet);
}

void StreamMemAccess::handlePacketResponse(LLVMTraceCPU *cpu,
                                           PacketPtr packet) {
  if (this->additionalDelay != 0) {
    // We have to reschedule the event to pay for the additional delay.
    STREAM_ELEMENT_DPRINTF(
        this->element, "PacketResponse with additional delay of %d cycles.\n",
        this->additionalDelay);
    auto responseEvent = new ResponseEvent(cpu, this, packet);
    cpu->schedule(responseEvent, cpu->clockEdge(Cycles(this->additionalDelay)));
    // Remember to reset the additional delay as we have already paid for it.
    this->additionalDelay = 0;
    return;
  }
  this->element->handlePacketResponse(this);
  // Check if this is a read request.
  if (packet->isRead()) {
    // We should notify the stream engine that this cache line is coming back.
    this->element->se->fetchedCacheBlock(this->cacheBlockVirtualAddr, this);
  }
  // After this point "this" is deleted.
  // Remember to release the packet.
  delete packet->req;
  delete packet;
  return;
}

void StreamMemAccess::handleStreamEngineResponse() {
  this->element->handlePacketResponse(this);
}

StreamElement::StreamElement(StreamEngine *_se) : se(_se) { this->clear(); }

void StreamElement::clear() {
  this->baseElements.clear();
  this->next = nullptr;
  this->stream = nullptr;
  this->FIFOIdx = FIFOEntryIdx();
  this->firstUserSeqNum = LLVMDynamicInst::INVALID_SEQ_NUM;
  this->isAddrReady = false;
  this->isValueReady = false;

  this->allocateCycle = Cycles(0);
  this->valueReadyCycle = Cycles(0);
  this->firstCheckCycle = Cycles(0);

  this->cacheBlocks = 0;
  this->size = 0;
  this->addr = 0;
  // Only clear the inflyMemAccess set, but not allocatedMemAccess set.
  this->inflyMemAccess.clear();
  this->stored = false;
}

StreamMemAccess *StreamElement::allocateStreamMemAccess(
    const CacheBlockBreakdownAccess &cacheBlockBreakdown) {
  auto memAccess = new StreamMemAccess(
      this->getStream(), this, cacheBlockBreakdown.cacheBlockVirtualAddr);
  this->allocatedMemAccess.insert(memAccess);
  /**
   * ! The reason why we allow such big number of allocated StreamMemAccess is
   * ! due to a pathological case: MemSet. In the current implementation, we
   * ! release the stream element not waiting for the writeback package to be
   * ! returned. In such case, we may run way ahead. However, this is rare in
   * ! the benchmarks.
   */
  if (this->allocatedMemAccess.size() == 100000) {
    STREAM_ELEMENT_PANIC(this, "Allocated 100000 StreamMemAccess.");
  }
  return memAccess;
}

void StreamElement::handlePacketResponse(StreamMemAccess *memAccess) {
  assert(this->allocatedMemAccess.count(memAccess) != 0 &&
         "This StreamMemAccess is not allocated by me.");

  if (this->inflyMemAccess.count(memAccess) != 0) {
    this->inflyMemAccess.erase(memAccess);
    if (this->inflyMemAccess.empty() && !this->isValueReady) {
      this->markValueReady();
    }
  }
  // Remember to release the memAccess.
  this->allocatedMemAccess.erase(memAccess);
  delete memAccess;
}

void StreamElement::markValueReady() {
  assert(!this->isValueReady && "Value is already ready.");
  this->isValueReady = true;
  this->valueReadyCycle = this->getStream()->getCPU()->curCycle();
  STREAM_ELEMENT_DPRINTF(this, "Value ready.\n");
}

void StreamElement::dump() const {}