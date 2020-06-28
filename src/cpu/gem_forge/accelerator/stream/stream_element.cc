#include "stream_element.hh"
#include "coalesced_stream.hh"
#include "stream.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "debug/StreamElement.hh"

#define DEBUG_TYPE StreamElement
#include "stream_log.hh"

StreamMemAccess::StreamMemAccess(Stream *_stream, StreamElement *_element,
                                 Addr _cacheBlockVAddr, Addr _vaddr, int _size,
                                 int _additionalDelay)
    : stream(_stream), element(_element), isReissue(_element->flushed),
      FIFOIdx(_element->FIFOIdx), cacheBlockVAddr(_cacheBlockVAddr),
      vaddr(_vaddr), size(_size), additionalDelay(_additionalDelay) {
  // Initialize it fairly simply.
  this->sliceId.streamId = this->FIFOIdx.streamId;
  this->sliceId.lhsElementIdx = this->FIFOIdx.entryIdx;
  this->sliceId.rhsElementIdx = this->FIFOIdx.entryIdx + 1;
  // ! So far always do cache line level.
  this->sliceId.vaddr = this->cacheBlockVAddr;
  this->sliceId.size = this->size;
}

void StreamMemAccess::registerReceiver(StreamElement *element) {
  // Sanity check that there are no duplicate receivers.
  for (int i = 0; i < this->numReceivers; ++i) {
    auto &receiver = this->receivers.at(i);
    if (receiver.first == element) {
      // It is possible that one element get reallocated.
      if (receiver.second) {
        S_ELEMENT_HACK(this->element, "Register receiver, my FIFOIdx is %s.\n",
                       this->FIFOIdx);
        S_ELEMENT_PANIC(element,
                        "Register duplicate receiver, still valid %d.\n",
                        this->receivers.at(i).second);
      } else {
        receiver.second = true;
        return;
      }
    }
  }
  if (this->numReceivers == StreamMemAccess::MAX_NUM_RECEIVERS) {
    for (int i = 0; i < this->numReceivers; ++i) {
      auto &receiver = this->receivers.at(i);
      if (receiver.second) {
        S_ELEMENT_HACK(receiver.first, "A valid receiver of [%#x, +%d).\n",
                       receiver.first->addr, receiver.first->size);
      } else {
        hack("In invalid receiver.\n");
      }
    }
    S_FIFO_ENTRY_PANIC(this->FIFOIdx, "Too many receivers.\n");
  }
  auto &newReceiver = this->receivers.at(this->numReceivers);
  newReceiver.first = element;
  newReceiver.second = true;
  this->numReceivers++;
}

void StreamMemAccess::deregisterReceiver(StreamElement *element) {
  for (int i = 0; i < this->numReceivers; ++i) {
    auto &receiver = this->receivers.at(i);
    if (receiver.first == element) {
      assert(receiver.second && "Receiver has already been deregistered.");
      receiver.second = false;
      return;
    }
  }
  assert(false && "Failed to find receiver.");
}

void StreamMemAccess::handlePacketResponse(PacketPtr pkt) {
  // API for stream-aware cache, as it doesn't have the cpu.
  this->handlePacketResponse(this->getStream()->getCPUDelegator(), pkt);
}

void StreamMemAccess::handlePacketResponse(GemForgeCPUDelegator *cpuDelegator,
                                           PacketPtr pkt) {
  if (this->additionalDelay != 0) {
    // We have to reschedule the event to pay for the additional delay.
    S_ELEMENT_DPRINTF(this->element,
                      "PacketResponse with additional delay of %d cycles.\n",
                      this->additionalDelay);
    auto responseEvent = new ResponseEvent(cpuDelegator, this, pkt);
    cpuDelegator->schedule(responseEvent, Cycles(this->additionalDelay));
    // Remember to reset the additional delay as we have already paid for it.
    this->additionalDelay = 0;
    return;
  }

  // Handle the request statistic.
  if (pkt->req->hasStatistic()) {
    bool hitInPrivateCache = false;
    auto statistic = pkt->req->getStatistic();
    switch (statistic->hitCacheLevel) {
    case RequestStatistic::HitPlaceE::INVALID: {
      // Invalid.
      break;
    }
    case RequestStatistic::HitPlaceE::MEM: // Hit in mem.
      this->stream->statistic.numMissL2++;
      this->stream->statistic.numMissL1++;
      this->stream->statistic.numMissL0++;
      hitInPrivateCache = false;
      break;
    case RequestStatistic::HitPlaceE::L1_STREAM_BUFFER:
      // This is considered hit in L2.
      this->stream->statistic.numMissL1++;
      this->stream->statistic.numMissL0++;
      hitInPrivateCache = false;
      break;
    case RequestStatistic::HitPlaceE::L2_CACHE:
      this->stream->statistic.numMissL1++;
      this->stream->statistic.numMissL0++;
      hitInPrivateCache = false;
      break;
    case RequestStatistic::HitPlaceE::L1_CACHE:
      this->stream->statistic.numMissL0++;
      hitInPrivateCache = true;
      break;
    case RequestStatistic::HitPlaceE::L0_CACHE: { // Hit in first level cache.
      hitInPrivateCache = true;
      break;
    }
    default: {
      panic("Invalid hitCacheLevel %d.\n", statistic->hitCacheLevel);
    }
    }
    // We just use the last dynamic stream.
    // Not 100% accurate but should be fine.
    if (this->stream->hasDynamicStream()) {
      auto &dynS = this->stream->getLastDynamicStream();
      dynS.recordHitHistory(hitInPrivateCache);
    }
  }

  // Check if this is a read request.
  if (pkt->isRead()) {
    // We should notify the stream engine that this cache line is coming back.
    this->element->se->fetchedCacheBlock(this->cacheBlockVAddr, this);
  }

  // Notify all receivers.
  for (int i = 0; i < this->numReceivers; ++i) {
    auto &receiver = this->receivers.at(i);
    if (receiver.second) {
      // The receiver is still expecting the response.
      receiver.first->handlePacketResponse(this, pkt);
      receiver.second = false;
    }
  }

  // Decrement myself as infly.
  this->stream->decrementInflyStreamRequest();
  this->stream->se->decrementInflyStreamRequest();

  // Release myself.
  delete this;
  delete pkt;

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

bool StreamElement::isLastElement() const {
  assert(this->dynS && "This element has not been allocated.");
  assert(this->dynS->configExecuted && "The DynS has not be configured.");
  return this->dynS->hasTotalTripCount() &&
         this->FIFOIdx.entryIdx == this->dynS->getTotalTripCount();
}

bool StreamElement::shouldIssue() const {
  /**
   * So far there are two cases when we do not issue requests:
   * 1. DynamicStream says so.
   * 2. LastElement that only uses to deal with StreamEnd.
   */
  if (!this->dynS->shouldCoreSEIssue()) {
    return false;
  }
  if (this->isLastElement()) {
    // Last element should never be issued.
    return false;
  }
  return true;
}

void StreamElement::clear() {

  if (this->FIFOIdx.entryIdx == 1) {
    if (this->FIFOIdx.streamId.coreId == 8 &&
        this->FIFOIdx.streamId.streamInstance == 1 &&
        this->FIFOIdx.streamId.staticId == 11704592) {
      S_ELEMENT_HACK(this, "Clear\n");
    }
  }

  this->addrBaseElements.clear();
  this->valueBaseElements.clear();
  this->next = nullptr;
  this->stream = nullptr;
  this->dynS = nullptr;
  this->FIFOIdx = FIFOEntryIdx();
  this->isCacheBlockedValue = false;
  this->firstUserSeqNum = LLVMDynamicInst::INVALID_SEQ_NUM;
  this->isStepped = false;
  this->isAddrReady = false;
  this->isAddrAliased = false;
  this->isValueReady = false;
  this->isCacheAcked = false;
  this->flushed = false;

  this->allocateCycle = Cycles(0);
  this->valueReadyCycle = Cycles(0);
  this->firstCheckCycle = Cycles(0);

  this->addr = 0;
  this->size = 0;
  this->clearCacheBlocks();
  std::fill(this->value.begin(), this->value.end(), 0);

  this->stored = false;
}

void StreamElement::clearCacheBlocks() {
  for (int i = 0; i < this->cacheBlocks; ++i) {
    auto &block = this->cacheBlockBreakdownAccesses[i];
    if (block.memAccess) {
      panic("Still has unregistered StreamMemAccess.");
    }
    block.clear();
  }
  this->cacheBlocks = 0;
}

void StreamElement::clearInflyMemAccesses() {
  // Deregister all StreamMemAccesses.
  for (int i = 0; i < this->cacheBlocks; ++i) {
    auto &block = this->cacheBlockBreakdownAccesses[i];
    if (block.memAccess) {
      block.memAccess->deregisterReceiver(this);
      block.memAccess = nullptr;
    }
  }
}

StreamMemAccess *StreamElement::allocateStreamMemAccess(
    const CacheBlockBreakdownAccess &cacheBlockBreakdown) {

  auto memAccess = new StreamMemAccess(
      this->getStream(), this, cacheBlockBreakdown.cacheBlockVAddr,
      cacheBlockBreakdown.virtualAddr, cacheBlockBreakdown.size);

  return memAccess;
}

void StreamElement::handlePacketResponse(StreamMemAccess *memAccess,
                                         PacketPtr pkt) {
  // Make sure I am still expect this.
  auto vaddr = memAccess->cacheBlockVAddr;
  auto size = pkt->getSize();
  auto blockIdx = this->mapVAddrToBlockOffset(vaddr, size);
  auto &block = this->cacheBlockBreakdownAccesses[blockIdx];
  assert(block.memAccess == memAccess &&
         "We are not expecting from this StreamMemAccess.");

  /**
   * Update the value vector.
   * Notice that pkt->getAddr() will give you physics address.
   * ! So far all requests are in cache line size.
   */
  auto data = pkt->getPtr<uint8_t>();
  /**
   * For atomic stream, we have to use the StreamAtomicOp' s LoadedValue.
   */
  auto S = this->stream;
  if (S->isAtomicStream() && pkt->isAtomicOp()) {
    auto atomicOp = pkt->getAtomicOp();
    auto streamAtomicOp = dynamic_cast<StreamAtomicOp *>(atomicOp);
    assert(streamAtomicOp && "Missing StreamAtomicOp.");
    auto loadedValue = streamAtomicOp->getLoadedValue();
    // * We should not use block addr/size for atomic op.
    this->setValue(memAccess->vaddr, S->getCoreElementSize(),
                   reinterpret_cast<uint8_t *>(&loadedValue));
  } else {
    this->setValue(vaddr, size, data);
  }

  // Clear the receiver.
  block.memAccess = nullptr;

  // Dummy way to check if this is a writeback mem access.
  for (auto &storeInstMemAccesses : this->inflyWritebackMemAccess) {
    storeInstMemAccesses.second.erase(memAccess);
  }
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

  GetStreamValueFunc getStreamValue =
      [this](uint64_t baseStreamId) -> uint64_t {
    auto baseStream = this->se->getStream(baseStreamId);
    for (auto baseElement : this->addrBaseElements) {
      if (baseElement->stream == baseStream) {
        // TODO: Check the FIFOIdx to make sure that the element is correct to
        // TODO: use.
        if (!baseElement->isValueReady) {
          S_ELEMENT_PANIC(this, "BaseElement %s is not value ready.",
                          baseElement->FIFOIdx);
        }
        auto vaddr = baseElement->addr;
        int32_t size = baseElement->size;
        if (auto CS = dynamic_cast<CoalescedStream *>(baseStream)) {
          // Handle offset for coalesced stream.
          int32_t offset;
          CS->getCoalescedOffsetAndSize(baseStreamId, offset, size);
          vaddr += offset;
        }
        // TODO: Fix this for reduction stream.
        assert(size <= sizeof(uint64_t) &&
               "Base element too large, maybe coalesced?");
        // ! This effectively does zero extension.
        uint64_t baseValue = 0;
        baseElement->getValue(vaddr, size,
                              reinterpret_cast<uint8_t *>(&baseValue));
        S_ELEMENT_DPRINTF(baseElement,
                          "GetStreamValue vaddr %#x size %d value %llu.\n",
                          vaddr, size, baseValue);
        return baseValue;
      }
    }
    /**
     * A special case for reduction stream: it is allowed to use an IVStream,
     * which should be the index of the LoadStream.
     */
    if (this->stream->isReduction()) {
      auto baseS = this->se->getStream(baseStreamId);
      assert(baseS->getStreamType() == ::LLVM::TDG::StreamInfo_Type_IV &&
             "Extra MemStream Input for ReductionStream.");
      auto &baseDynS = baseS->getDynamicStream(this->dynS->configSeqNum);
      assert(baseDynS.configExecuted && "Extra IVBaseStream is configured.");
      // It should have -1 ElementIdx.
      assert(this->FIFOIdx.entryIdx > 0 &&
             "Generate value for first element of ReductionStream.");
      // This IVBaseStream should simply has no input.
      return baseDynS.addrGenCallback->genAddr(this->FIFOIdx.entryIdx - 1,
                                               baseDynS.addrGenFormalParams,
                                               getStreamValueFail);
    }
    S_ELEMENT_PANIC(this, "Failed to find the base stream value of %llu.\n",
                    baseStreamId);
  };

  if (this->stream->isReduction() && this->FIFOIdx.entryIdx == 0) {
    // Special case: first element of reduction stream uses the initial value.
    this->addr = dynStream.initialValue;
  } else if (this->isLastElement() && this->stream->isReduction() &&
             !this->stream->hasCoreUser() && this->dynS->offloadedToCache) {
    // Special case: last element of offloaded reduction stream without core
    // user.
    assert(this->dynS->finalReductionValueReady &&
           "FinalReductionValue should be ready.");
    this->addr = this->dynS->finalReductionValue;
  } else {
    // Normal case: use addrGenCallback.
    this->addr = dynStream.addrGenCallback->genAddr(
        this->FIFOIdx.entryIdx, dynStream.addrGenFormalParams, getStreamValue);
  }

  this->size = stream->getMemElementSize();

  S_ELEMENT_DPRINTF(this, "MarkAddrReady vaddr %#x size %d.\n", this->addr,
                    this->size);

  // 3. Split into cache lines.
  this->splitIntoCacheBlocks(cpuDelegator);
}

void StreamElement::tryMarkValueReady() {
  for (int blockIdx = 0; blockIdx < this->cacheBlocks; ++blockIdx) {
    const auto &block = this->cacheBlockBreakdownAccesses[blockIdx];
    if (block.state != CacheBlockBreakdownAccess::StateE::Ready &&
        block.state != CacheBlockBreakdownAccess::StateE::Faulted) {
      return;
    }
  }
  this->markValueReady();
}

void StreamElement::markValueReady() {
  assert(!this->isValueReady && "Value is already ready.");
  this->isValueReady = true;
  this->valueReadyCycle = this->getStream()->getCPUDelegator()->curCycle();
  S_ELEMENT_DPRINTF(this, "Value ready.\n");

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
    newCacheBlockBreakdown.state =
        CacheBlockBreakdownAccess::StateE::Initialized;
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
    if (block.state != CacheBlockBreakdownAccess::StateE::PrevElement) {
      continue;
    }
    // Get previous block.
    auto prevBlockOffset = prevElement->mapVAddrToBlockOffset(
        block.cacheBlockVAddr, this->cacheBlockSize);
    const auto &prevBlock =
        prevElement->cacheBlockBreakdownAccesses[prevBlockOffset];
    if (prevBlock.state == CacheBlockBreakdownAccess::StateE::Faulted) {
      // Propagate the faulted state.
      block.state = CacheBlockBreakdownAccess::StateE::Faulted;
      this->tryMarkValueReady();
      continue;
    }
    auto offset = prevElement->mapVAddrToValueOffset(block.cacheBlockVAddr,
                                                     this->cacheBlockSize);
    // Copy the value from prevElement.
    this->setValue(block.cacheBlockVAddr, this->cacheBlockSize,
                   &prevElement->value.at(offset));
    assert(block.state == CacheBlockBreakdownAccess::StateE::Ready);
  }
}

void StreamElement::setValue(Addr vaddr, int size, const uint8_t *val) {
  // Copy the data.
  auto initOffset = this->mapVAddrToValueOffset(vaddr, size);
  if (Debug::DEBUG_TYPE) {
    std::stringstream ss;
    for (auto i = 0; i < size; ++i) {
      ss << ' ' << std::hex << static_cast<int>(val[i]) << std::dec;
    }
    S_ELEMENT_DPRINTF(this, "SetValue [%#x, %#x), initOffset %d, data 0x%s.\n",
                      vaddr, vaddr + size, initOffset, ss.str());
  }
  for (int i = 0; i < size; ++i) {
    this->value.at(i + initOffset) = val[i];
  }
  // Mark the cache line ready.
  // TODO: Really check that every byte is set.
  for (int blockIdx = 0; blockIdx < this->cacheBlocks; ++blockIdx) {
    auto &block = this->cacheBlockBreakdownAccesses[blockIdx];
    // So far we just check for overlap.
    if (vaddr >= block.cacheBlockVAddr + this->cacheBlockSize ||
        vaddr + size <= block.cacheBlockVAddr) {
      // No overlap.
      continue;
    }
    S_ELEMENT_DPRINTF(this, "Mark block ready: [%#x, %#x).\n",
                      block.cacheBlockVAddr,
                      block.cacheBlockVAddr + this->cacheBlockSize);
    block.state = CacheBlockBreakdownAccess::StateE::Ready;
  }

  this->tryMarkValueReady();
}

void StreamElement::getValue(Addr vaddr, int size, uint8_t *val) const {
  // Copy the data.
  auto initOffset = this->mapVAddrToValueOffset(vaddr, size);
  if (Debug::DEBUG_TYPE) {
    std::stringstream ss;
    for (auto i = 0; i < size; ++i) {
      ss << ' ' << std::hex << static_cast<int>(this->value.at(i + initOffset))
         << std::dec;
    }
    S_ELEMENT_DPRINTF(this, "GetValue [%#x, %#x), initOffset %d, data 0x%s.\n",
                      vaddr, vaddr + size, initOffset, ss.str());
  }
  for (int i = 0; i < size; ++i) {
    val[i] = this->value.at(i + initOffset);
  }
}

void StreamElement::getValueByStreamId(StaticId streamId, uint8_t *val,
                                       int valLen) const {
  auto vaddr = this->addr;
  int size = this->size;
  // Handle offset for coalesced stream.
  int32_t offset;
  auto CS = dynamic_cast<CoalescedStream *>(this->stream);
  assert(CS && "All stream should be Coalesced stream now.");
  CS->getCoalescedOffsetAndSize(streamId, offset, size);
  assert(size <= valLen && "ElementSize overflow.");
  vaddr += offset;
  this->getValue(vaddr, size, val);
}

bool StreamElement::isValueFaulted(Addr vaddr, int size) const {
  auto blockIdx = this->mapVAddrToBlockOffset(vaddr, size);
  auto blockEnd = this->mapVAddrToBlockOffset(vaddr + size - 1, 1);
  while (blockIdx <= blockEnd) {
    const auto &block = this->cacheBlockBreakdownAccesses[blockIdx];
    if (block.state == CacheBlockBreakdownAccess::Faulted) {
      return true;
    }
    blockIdx++;
  }
  return false;
}

bool StreamElement::areValueBaseElementsValueReady() const {
  for (const auto &baseE : this->valueBaseElements) {
    if (!baseE.isValid()) {
      S_ELEMENT_PANIC(this, "ValueBaseElement released early: %s.", baseE.idx);
    }
    if (!baseE.element->isValueReady) {
      return false;
    }
  }
  return true;
}

uint64_t StreamElement::mapVAddrToValueOffset(Addr vaddr, int size) const {
  assert(this->cacheBlocks > 0 && "There is no cache blocks.");
  auto firstCacheBlockVAddr =
      this->cacheBlockBreakdownAccesses[0].cacheBlockVAddr;
  if (vaddr < firstCacheBlockVAddr) {
    S_ELEMENT_PANIC(this, "Underflow of vaddr %#x, [%#x, +%d).", vaddr,
                    this->addr, this->size);
  }
  auto initOffset = vaddr - firstCacheBlockVAddr;
  assert(initOffset + size <= this->value.size() && "Overflow of size.");
  return initOffset;
}

uint64_t StreamElement::mapVAddrToBlockOffset(Addr vaddr, int size) const {
  return this->mapVAddrToValueOffset(vaddr, size) / this->cacheBlockSize;
}

void StreamElement::dump() const {
  inform("Stream %50s %d.%d (%d%d).\n", this->stream->getStreamName().c_str(),
         this->FIFOIdx.streamId.streamInstance, this->FIFOIdx.entryIdx,
         static_cast<int>(this->isAddrReady),
         static_cast<int>(this->isValueReady));
}