#include "MLCDynamicDirectStream.hh"
#include "MLCDynamicIndirectStream.hh"

// Generated by slicc.
#include "mem/ruby/protocol/CoherenceMsg.hh"
#include "mem/ruby/protocol/RequestMsg.hh"
#include "mem/simple_mem.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStream.hh"

#define DEBUG_TYPE MLCRubyStream
#include "../stream_log.hh"

MLCDynamicDirectStream::MLCDynamicDirectStream(
    CacheStreamConfigureData *_configData,
    AbstractStreamAwareController *_controller,
    MessageBuffer *_responseMsgBuffer, MessageBuffer *_requestToLLCMsgBuffer,
    MLCDynamicIndirectStream *_indirectStream)
    : MLCDynamicStream(_configData, _controller, _responseMsgBuffer,
                       _requestToLLCMsgBuffer),
      slicedStream(_configData, true /* coalesceContinuousElements */),
      llcTailSliceIdx(0), indirectStream(_indirectStream) {

  // Initialize the llc bank.
  assert(_configData->initPAddrValid && "InitPAddr should be valid.");
  this->tailPAddr = _configData->initPAddr;
  this->tailSliceLLCBank = this->mapPAddrToLLCBank(_configData->initPAddr);

  // Initialize the buffer for some slices.
  // Since the LLC is bounded by the credit, it's sufficient to only check
  // hasOverflowed() at MLC level.
  while (this->tailSliceIdx < this->maxNumSlices &&
         !this->slicedStream.hasOverflowed()) {
    this->allocateSlice();
  }

  this->llcTailSliceIdx = this->tailSliceIdx;
  this->llcTailPAddr = this->tailPAddr;
  this->llcTailSliceLLCBank = this->tailSliceLLCBank;

  // Set the CacheStreamConfigureData to inform the LLC stream engine
  // initial credit.
  _configData->initAllocatedIdx = this->llcTailSliceIdx;

  MLC_S_DPRINTF("InitAllocatedSlice %d overflowed %d.\n", this->llcTailSliceIdx,
                this->slicedStream.hasOverflowed());
}

void MLCDynamicDirectStream::advanceStream() {

  this->popStream();
  // Of course we need to allocate more slices.
  while (this->tailSliceIdx - this->headSliceIdx < this->maxNumSlices &&
         !this->hasOverflowed()) {
    this->allocateSlice();
  }

  // We may need to schedule advance stream if the first slice is FAULTED,
  // as no other event will cause it to be released.
  if (!this->slices.empty() &&
      this->slices.front().coreStatus == MLCStreamSlice::CoreStatusE::FAULTED) {
    if (!this->advanceStreamEvent.scheduled()) {
      this->stream->getCPUDelegator()->schedule(&this->advanceStreamEvent,
                                                Cycles(1));
    }
  }

  /**
   * There are two cases we need to send the token:
   * 1. We have allocated more half the buffer size.
   * 2. The stream has overflowed.
   * 3. The llc stream is cutted.
   */
  if (!this->llcCutted) {
    if (!this->slicedStream.hasOverflowed()) {
      if (this->tailSliceIdx - this->llcTailSliceIdx > this->maxNumSlices / 2) {
        this->sendCreditToLLC();
      }
    } else {
      if (this->tailSliceIdx > this->llcTailSliceIdx) {
        this->sendCreditToLLC();
      }
    }
  } else {
    if (this->llcCutSliceIdx > this->llcTailSliceIdx) {
      this->sendCreditToLLC();
    }
  }
}

void MLCDynamicDirectStream::allocateSlice() {
  auto sliceId = this->slicedStream.getNextSlice();
  MLC_SLICE_DPRINTF(sliceId, "Allocated %#x.\n", sliceId.vaddr);

  this->slices.emplace_back(sliceId);
  this->stream->statistic.numMLCAllocatedSlice++;

  // Update the llc cut information.
  if (this->llcCutLineVAddr == sliceId.vaddr) {
    // This should be cutted.
    this->llcCutSliceIdx = this->tailSliceIdx;
    this->llcCutted = true;
  }

  // Try to handle faulted slice.
  Addr paddr;
  auto cpuDelegator = this->getStaticStream()->getCPUDelegator();
  if (cpuDelegator->translateVAddrOracle(sliceId.vaddr, paddr)) {
    /**
     * This address is valid.
     * Check if we have reuse data.
     */
    auto reuseIter = this->reuseBlockMap.find(sliceId.vaddr);
    if (reuseIter != this->reuseBlockMap.end()) {
      this->slices.back().setData(reuseIter->second);
      this->reuseBlockMap.erase(reuseIter);
    }

    /**
     * ! We cheat here to notify the indirect stream immediately,
     * ! to avoid some complicate problem of managing streams.
     */
    assert(this->controller->params()->ruby_system->getAccessBackingStore() &&
           "This only works with backing store.");
    this->notifyIndirectStream(this->slices.back());

  } else {
    // This address is invalid. Mark the slice faulted.
    this->slices.back().coreStatus = MLCStreamSlice::CoreStatusE::FAULTED;
  }

  // Try to find where the LLC stream would be at this point.
  this->tailSliceIdx++;
  if (cpuDelegator->translateVAddrOracle(
          this->slicedStream.peekNextSlice().vaddr, paddr)) {
    // The next slice would be valid.
    this->tailPAddr = paddr;
    this->tailSliceLLCBank = this->mapPAddrToLLCBank(paddr);

  } else {
    // This address is invalid.
    // Do not update tailSliceLLCBank as the LLC stream would not move.
  }
}

void MLCDynamicDirectStream::sendCreditToLLC() {
  /**
   * The LLC stream will be at llcTailSliceLLCBank, and we need to
   * update its credit and the new location is tailSliceLLCBank.
   *
   * This will not work for pointer chasing stream.
   */
  assert(this->tailSliceIdx > this->llcTailSliceIdx &&
         "Don't know where to send credit.");

  // Send the flow control.
  MLC_S_DPRINTF("Extended %lu -> %lu, sent credit to LLC%d.\n",
                this->llcTailSliceIdx, this->tailSliceIdx,
                this->llcTailSliceLLCBank.num);
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = this->llcTailPAddr;
  msg->m_Type = CoherenceRequestType_STREAM_FLOW;
  msg->m_Requestor = this->controller->getMachineID();
  msg->m_Destination.add(this->llcTailSliceLLCBank);
  msg->m_MessageSize = MessageSizeType_Control;
  msg->m_sliceId.streamId = this->dynamicStreamId;
  msg->m_sliceId.lhsElementIdx = this->llcTailSliceIdx;
  msg->m_sliceId.rhsElementIdx = this->tailSliceIdx;

  Cycles latency(1); // Just use 1 cycle latency here.

  this->requestToLLCMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(latency));

  // Update the record.
  this->llcTailSliceIdx = this->tailSliceIdx;
  this->llcTailPAddr = this->tailPAddr;
  this->llcTailSliceLLCBank = this->tailSliceLLCBank;
}

void MLCDynamicDirectStream::receiveStreamData(const ResponseMsg &msg) {
  const auto &sliceId = msg.m_sliceId;
  assert(sliceId.isValid() && "Invalid stream slice id for stream data.");

  auto numElements = sliceId.getNumElements();
  assert(this->dynamicStreamId == sliceId.streamId &&
         "Unmatched dynamic stream id.");
  MLC_SLICE_DPRINTF(sliceId, "Receive data %#x.\n", sliceId.vaddr);

  /**
   * It is possible when the core stream engine runs ahead than
   * the LLC stream engine, and the stream data is delivered after
   * the slice is released. In such case we will ignore the
   * stream data.
   *
   * TODO: Properly handle this with sliceIdx.
   */
  if (this->slices.empty()) {
    assert(this->hasOverflowed() && "No slices when not overflowed yet.");
    // Simply ignore it.
    return;
  } else {
    // TODO: Properly detect that the slice is lagging behind.
    const auto &firstSlice = this->slices.front();
    bool laggingBehind = false;
    if (sliceId.lhsElementIdx < firstSlice.sliceId.lhsElementIdx) {
      laggingBehind = true;
    }
    if (sliceId.lhsElementIdx == firstSlice.sliceId.lhsElementIdx &&
        sliceId.vaddr < firstSlice.sliceId.vaddr) {
      // Due to multi-line elements, we have to also check vaddr.
      laggingBehind = true;
    }
    if (laggingBehind) {
      // The stream data is lagging behind. The slice is already
      // released.
      MLC_SLICE_DPRINTF(sliceId, "Discard as lagging behind %s.\n",
                        firstSlice.sliceId);
      return;
    }
  }

  /**
   * Find the correct stream slice and insert the data there.
   * Here we reversely search for it to save time.
   */
  for (auto slice = this->slices.rbegin(), end = this->slices.rend();
       slice != end; ++slice) {
    if (this->matchSliceId(slice->sliceId, sliceId)) {
      // Found the slice.
      if (slice->sliceId.getNumElements() != numElements) {
        // Also consider llc stream being cut.
        if (this->llcCutLineVAddr > 0 &&
            slice->sliceId.vaddr < this->llcCutLineVAddr) {
          MLC_S_PANIC("Mismatch numElements, incoming %d, slice %d.\n",
                      numElements, slice->sliceId.getNumElements());
        }
      }
      if (slice->dataReady) {
        // Must be from reuse.
      } else {
        slice->setData(msg.m_DataBlk);
      }

      // // Notify the indirect stream. Call this after setData().
      // this->notifyIndirectStream(*slice);

      if (slice->coreStatus == MLCStreamSlice::CoreStatusE::WAIT) {
        this->makeResponse(*slice);
      }
      this->advanceStream();
      return;
    }
  }

  MLC_SLICE_PANIC(sliceId, "Fail to find the slice. Tail %lu.\n",
                  this->tailSliceIdx);
}

void MLCDynamicDirectStream::notifyIndirectStream(const MLCStreamSlice &slice) {

  if (!this->indirectStream) {
    return;
  }

  const auto &sliceId = slice.sliceId;
  MLC_SLICE_DPRINTF(sliceId, "Notify IndirectSream.\n");
  for (auto elementIdx = sliceId.lhsElementIdx;
       elementIdx < sliceId.rhsElementIdx; ++elementIdx) {

    // Try to extract the stream data.
    auto elementVAddr = this->slicedStream.getElementVAddr(elementIdx);
    auto elementSize = this->slicedStream.getElementSize();
    auto elementLineOffset = elementVAddr % RubySystem::getBlockSizeBytes();
    assert(elementLineOffset + elementSize <= RubySystem::getBlockSizeBytes() &&
           "Cannot support multi-line element with indirect streams yet.");
    assert(elementSize <= sizeof(uint64_t) && "At most 8 byte element size.");

    uint64_t elementData = 0;
    auto rubySystem = this->controller->params()->ruby_system;
    if (rubySystem->getAccessBackingStore()) {
      // Get the data from backing store.
      Addr elementPAddr = this->translateVAddr(elementVAddr);
      RequestPtr req(new Request(elementPAddr, elementSize, 0, 0 /* MasterId */,
                                 0 /* InstSeqNum */, 0 /* contextId */));
      PacketPtr pkt = Packet::createRead(req);
      uint8_t *pktData = new uint8_t[req->getSize()];
      pkt->dataDynamic(pktData);
      rubySystem->getPhysMem()->functionalAccess(pkt);
      for (auto byteOffset = 0; byteOffset < elementSize; ++byteOffset) {
        *(reinterpret_cast<uint8_t *>(&elementData) + byteOffset) =
            pktData[byteOffset];
      }
      delete pkt;
    } else {
      // Get the data from the cache line.
      // Copy the data in.
      // TODO: How do we handle sign for data type less than 8 bytes?
      for (auto byteOffset = 0; byteOffset < elementSize; ++byteOffset) {
        *(reinterpret_cast<uint8_t *>(&elementData) + byteOffset) =
            slice.dataBlock.getByte(byteOffset + elementLineOffset);
      }
    }
    MLC_SLICE_DPRINTF(sliceId, "Extract element %lu data %#x.\n", elementIdx,
                      elementData);
    this->indirectStream->receiveBaseStreamData(elementIdx, elementData);
  }
}

MLCDynamicDirectStream::SliceIter
MLCDynamicDirectStream::findSliceForCoreRequest(
    const DynamicStreamSliceId &sliceId) {
  if (this->slices.empty()) {
    MLC_S_PANIC("No slices for request, overflowed %d, totalTripCount %lu.\n",
                this->hasOverflowed(), this->getTotalTripCount());
  }
  // Try to allocate more slices.
  while (!this->hasOverflowed() &&
         this->slices.back().sliceId.lhsElementIdx <= sliceId.lhsElementIdx) {
    this->allocateSlice();
  }
  for (auto iter = this->slices.begin(), end = this->slices.end(); iter != end;
       ++iter) {
    /**
     * So far we match them on vaddr.
     * TODO: Really assign the sliceIdx and match that.
     */
    if (iter->sliceId.vaddr == sliceId.vaddr) {
      return iter;
    }
  }

  MLC_S_PANIC("Failed to find slice for core %s.\n", sliceId);
}

void MLCDynamicDirectStream::receiveReuseStreamData(
    Addr vaddr, const DataBlock &dataBlock) {
  MLC_S_DPRINTF("Received reuse block %#x.\n", vaddr);
  this->reuseBlockMap.emplace(vaddr, dataBlock).second;
  /**
   * TODO: The current implementation may have multiple reuses, in
   * TODO: the boundary cases when streams overlapped.
   */
}
