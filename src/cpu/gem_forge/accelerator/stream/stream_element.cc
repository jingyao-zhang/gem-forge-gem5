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
                 element->FIFOIdx.streamId.streamInstance,                     \
                 element->FIFOIdx.entryIdx, ##args)

#define STREAM_ELEMENT_PANIC(element, format, args...)                         \
  element->se->dump();                                                         \
  STREAM_PANIC(element->getStream(), "[%lu, %lu]: " format,                    \
               element->FIFOIdx.streamId.streamInstance,                       \
               element->FIFOIdx.entryIdx, ##args)

FIFOEntryIdx::FIFOEntryIdx()
    : streamId(), configSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM), entryIdx(0) {}

FIFOEntryIdx::FIFOEntryIdx(const DynamicStreamId &_streamId)
    : streamId(_streamId), configSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
      entryIdx(0) {}

StreamMemAccess::StreamMemAccess(Stream *_stream, StreamElement *_element,
                                 Addr _cacheBlockVAddr, Addr _vaddr, int _size,
                                 int _additionalDelay)
    : stream(_stream), element(_element), FIFOIdx(_element->FIFOIdx),
      cacheBlockVAddr(_cacheBlockVAddr), vaddr(_vaddr), size(_size),
      additionalDelay(_additionalDelay) {}

const DynamicStreamId &StreamMemAccess::getDynamicStreamId() const {
  return this->FIFOIdx.streamId;
}

DynamicStreamSliceId StreamMemAccess::getSliceId() const {
  DynamicStreamSliceId slice;
  slice.streamId = this->FIFOIdx.streamId;
  slice.startIdx = this->FIFOIdx.entryIdx;
  // So far we make it fairly simple here.
  slice.endIdx = slice.startIdx + 1;
  return slice;
}

void StreamMemAccess::handlePacketResponse(PacketPtr pkt) {
  // API for stream-aware cache, as it doesn't have the cpu.
  this->handlePacketResponse(this->getStream()->getCPUDelegator(), pkt);
}

void StreamMemAccess::handlePacketResponse(GemForgeCPUDelegator *cpuDelegator,
                                           PacketPtr pkt) {
  if (this->additionalDelay != 0) {
    // We have to reschedule the event to pay for the additional delay.
    STREAM_ELEMENT_DPRINTF(
        this->element, "PacketResponse with additional delay of %d cycles.\n",
        this->additionalDelay);
    auto responseEvent = new ResponseEvent(cpuDelegator, this, pkt);
    cpuDelegator->schedule(responseEvent, Cycles(this->additionalDelay));
    // Remember to reset the additional delay as we have already paid for it.
    this->additionalDelay = 0;
    return;
  }

  // Handle the request statistic.
  if (pkt->req->hasStatistic()) {
    auto statistic = pkt->req->getStatistic();
    switch (statistic->hitCacheLevel) {
    case RequestStatistic::HitPlaceE::INVALID: {
      // Invalid.
      break;
    }
    case RequestStatistic::HitPlaceE::MEM: // Hit in mem.
      this->stream->statistic.numMissL2++;
      [[gnu::fallthrough]];
    case RequestStatistic::HitPlaceE::L1_STREAM_BUFFER:
      // This is considered hit in L2.
      [[gnu::fallthrough]];
    case RequestStatistic::HitPlaceE::L2_CACHE:
      this->stream->statistic.numMissL1++;
      [[gnu::fallthrough]];
    case RequestStatistic::HitPlaceE::L1_CACHE:
      this->stream->statistic.numMissL0++;
      break;
    case RequestStatistic::HitPlaceE::L0_CACHE: { // Hit in first level cache.
      break;
    }
    default: { panic("Invalid hitCacheLevel %d.\n", statistic->hitCacheLevel); }
    }
  }

  // Check if this is a read request.
  if (pkt->isRead()) {
    // We should notify the stream engine that this cache line is coming back.
    this->element->se->fetchedCacheBlock(this->cacheBlockVAddr, this);
  }
  this->element->handlePacketResponse(this, pkt);
  /*********************************************************************
   * ! After StreamElement::handlePacketResponse() this is deleted.
   *********************************************************************/
  return;
}

void StreamMemAccess::issueToMemoryCallback(
    GemForgeCPUDelegator *cpuDelegator) {
  this->element->issueCycle = cpuDelegator->curCycle();
}

void StreamMemAccess::handleStreamEngineResponse() {
  // Merge at StreamEngine level is disabled as we have no easy method
  // to propagate the data here.
  assert(false && "No propagate data for merged request.");
  this->element->handlePacketResponse(this, nullptr);
}

StreamElement::StreamElement(StreamEngine *_se) : se(_se) { this->clear(); }

void StreamElement::clear() {
  this->baseElements.clear();
  this->next = nullptr;
  this->stream = nullptr;
  this->FIFOIdx = FIFOEntryIdx();
  this->firstUserSeqNum = LLVMDynamicInst::INVALID_SEQ_NUM;
  this->isStepped = false;
  this->isAddrReady = false;
  this->isValueReady = false;
  this->flushed = false;

  this->allocateCycle = Cycles(0);
  this->valueReadyCycle = Cycles(0);
  this->firstCheckCycle = Cycles(0);

  this->addr = 0;
  this->size = 0;
  this->cacheBlocks = 0;
  std::fill(this->value.begin(), this->value.end(), 0);

  // Only clear the inflyMemAccess set, but not allocatedMemAccess set.
  this->inflyMemAccess.clear();
  this->stored = false;
  this->markNextElementValueReady = false;
}

StreamMemAccess *StreamElement::allocateStreamMemAccess(
    const CacheBlockBreakdownAccess &cacheBlockBreakdown) {

  auto memAccess = new StreamMemAccess(
      this->getStream(), this, cacheBlockBreakdown.cacheBlockVAddr,
      cacheBlockBreakdown.virtualAddr, cacheBlockBreakdown.size);

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

void StreamElement::handlePacketResponse(StreamMemAccess *memAccess,
                                         PacketPtr pkt) {
  assert(this->allocatedMemAccess.count(memAccess) != 0 &&
         "This StreamMemAccess is not allocated by me.");

  if (this->inflyMemAccess.count(memAccess) != 0) {

    /**
     * Update the value vector.
     * Notice that pkt->getAddr() will give you physics address.
     * ! So far all requests are in cache line size.
     */
    auto vaddr = memAccess->cacheBlockVAddr;
    auto size = pkt->getSize();
    auto data = pkt->getPtr<uint8_t>();
    this->setValue(vaddr, size, data);

    this->inflyMemAccess.erase(memAccess);
    if (this->inflyMemAccess.empty() && !this->isValueReady) {
      this->markValueReady();
    }
  }
  // Dummy way to check if this is a writeback mem access.
  for (auto &storeInstMemAccesses : this->inflyWritebackMemAccess) {
    storeInstMemAccesses.second.erase(memAccess);
  }
  // Remember to release the memAccess.
  this->allocatedMemAccess.erase(memAccess);
  delete memAccess;
  // Remember to release the pkt.
  delete pkt;
}

bool StreamElement::isFirstUserDispatched() const {
  return this->firstUserSeqNum != ::LLVMDynamicInst::INVALID_SEQ_NUM;
}

void StreamElement::markAddrReady(GemForgeCPUDelegator *cpuDelegator) {
  assert(!this->isAddrReady && "Addr is already ready.");
  this->isAddrReady = true;
  this->addrReadyCycle = cpuDelegator->curCycle();

  /**
   * Compute the address.
   */
  auto &dynStream = this->stream->getDynamicStream(this->FIFOIdx.configSeqNum);

  // 1. Prepare the parameters.
  std::vector<uint64_t> params;
  for (const auto &formalParam : dynStream.formalParams) {
    if (formalParam.isInvariant) {
      params.push_back(formalParam.param.invariant);
    } else {
      auto baseStreamId = formalParam.param.baseStreamId;
      auto baseStream = this->se->getStream(baseStreamId);
      bool foundBaseValue = false;
      for (auto baseElement : this->baseElements) {
        if (baseElement->stream == baseStream) {
          // TODO: Check the FIFOIdx to make sure that the element is correct to
          // TODO: use.
          assert(baseElement->isValueReady &&
                 "Base element is not value ready yet.");
          assert(baseElement->size <= sizeof(uint64_t) &&
                 "Base element too large, maybe coalesced?");
          // ! This effectively does zero extension.
          uint64_t baseValue = 0;
          baseElement->getValue(baseElement->addr, baseElement->size,
                                reinterpret_cast<uint8_t *>(&baseValue));
          params.push_back(baseValue);
          foundBaseValue = true;
          break;
        }
      }
      assert(foundBaseValue && "Failed to find the base stream value.");
    }
  }

  // 2. Call the AddrGenCallback.
  this->addr =
      dynStream.addrGenCallback->genAddr(this->FIFOIdx.entryIdx, params);
  this->size = stream->getElementSize();

  // 3. Split into cache lines.
  this->splitIntoCacheBlocks(cpuDelegator);
}

void StreamElement::markValueReady() {
  assert(!this->isValueReady && "Value is already ready.");
  this->isValueReady = true;
  this->valueReadyCycle = this->getStream()->getCPUDelegator()->curCycle();
  STREAM_ELEMENT_DPRINTF(this, "Value ready.\n");

  // Check if the next element is also dependent on me.
  if (this->markNextElementValueReady) {
    if (this->next != nullptr &&
        this->next->FIFOIdx.streamId == this->FIFOIdx.streamId &&
        this->next->FIFOIdx.entryIdx == this->FIFOIdx.entryIdx + 1) {
      // Okay the next element is still valid.
      assert(this->next->isAddrReady &&
             "Address should be ready to propagate the value ready signal.");

      // Fill next element cache blocks.
      this->next->setValue(this);
      this->next->markValueReady();
    }
  }

  // Notify the stream for statistics.
  if (this->issueCycle >= this->addrReadyCycle &&
      this->issueCycle <= this->valueReadyCycle) {
    // The issue cycle is valid.
    this->stream->statistic.numCycleRequestLatency +=
        this->valueReadyCycle - this->issueCycle;
  }
}

void StreamElement::splitIntoCacheBlocks(GemForgeCPUDelegator *cpuDelegator) {
  // TODO: Initialize this only once.
  this->cacheBlockSize = cpuDelegator->cacheLineSize();

  for (int currentSize, totalSize = 0; totalSize < this->size;
       totalSize += currentSize) {
    if (this->cacheBlocks >= StreamElement::MAX_CACHE_BLOCKS) {
      panic("More than %d cache blocks for one stream element, address %lu "
            "size %lu.",
            this->cacheBlocks, this->addr, this->size);
    }
    auto currentAddr = this->addr + totalSize;
    currentSize = this->size - totalSize;
    // Make sure we don't span across multiple cache blocks.
    if (((currentAddr % cacheBlockSize) + currentSize) > cacheBlockSize) {
      currentSize = cacheBlockSize - (currentAddr % cacheBlockSize);
    }
    // Create the breakdown.
    auto cacheBlockAddr = currentAddr & (~(cacheBlockSize - 1));
    auto &newCacheBlockBreakdown =
        this->cacheBlockBreakdownAccesses[this->cacheBlocks];
    newCacheBlockBreakdown.cacheBlockVAddr = cacheBlockAddr;
    newCacheBlockBreakdown.virtualAddr = currentAddr;
    newCacheBlockBreakdown.size = currentSize;
    this->cacheBlocks++;
  }

  // Expand the value to match the number of cache blocks.
  // We never shrink this value vector.
  auto cacheBlockBytes = this->cacheBlocks * cacheBlockSize;
  while (this->value.size() < cacheBlockBytes) {
    this->value.push_back(0);
  }
}

void StreamElement::setValue(StreamElement *prevElement) {
  // Fill element cache blocks from previous element.
  // This should be completely overlapped.
  assert(prevElement->next == this && "Next element should be me.");
  for (int blockIdx = 0; blockIdx < this->cacheBlocks; ++blockIdx) {
    auto &block = this->cacheBlockBreakdownAccesses[blockIdx];
    assert(this->cacheBlockSize > 0 && "cacheBlockSize is not initialized.");
    auto initOffset = this->mapVAddrToValueOffset(block.cacheBlockVAddr,
                                                  this->cacheBlockSize);
    // Get the value from previous element.
    prevElement->getValue(block.cacheBlockVAddr, this->cacheBlockSize,
                          this->value.data() + initOffset);
  }
}

void StreamElement::setValue(Addr vaddr, int size, const uint8_t *val) {
  // Copy the data.
  auto initOffset = this->mapVAddrToValueOffset(vaddr, size);
  for (int i = 0; i < size; ++i) {
    this->value.at(i + initOffset) = val[i];
  }
}

void StreamElement::getValue(Addr vaddr, int size, uint8_t *val) const {
  // Copy the data.
  auto initOffset = this->mapVAddrToValueOffset(vaddr, size);
  for (int i = 0; i < size; ++i) {
    val[i] = this->value.at(i + initOffset);
  }
}

uint64_t StreamElement::mapVAddrToValueOffset(Addr vaddr, int size) const {
  assert(this->cacheBlocks > 0 && "There is no cache blocks.");
  auto firstCacheBlockVAddr =
      this->cacheBlockBreakdownAccesses[0].cacheBlockVAddr;
  assert(vaddr >= firstCacheBlockVAddr && "Underflow of vaddr.");
  auto initOffset = vaddr - firstCacheBlockVAddr;
  assert(initOffset + size <= this->value.size() && "Overflow of size.");
  return initOffset;
}

void StreamElement::dump() const {
  inform("Stream %50s %d.%d (%d%d).\n", this->stream->getStreamName().c_str(),
         this->FIFOIdx.streamId.streamInstance, this->FIFOIdx.entryIdx,
         static_cast<int>(this->isAddrReady),
         static_cast<int>(this->isValueReady));
}