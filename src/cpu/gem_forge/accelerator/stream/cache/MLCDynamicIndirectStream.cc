#include "MLCDynamicIndirectStream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStreamBase.hh"
#include "debug/MLCRubyStreamReduce.hh"

#define DEBUG_TYPE MLCRubyStreamBase
#include "../stream_log.hh"

MLCDynamicIndirectStream::MLCDynamicIndirectStream(
    CacheStreamConfigureDataPtr _configData,
    AbstractStreamAwareController *_controller,
    MessageBuffer *_responseMsgBuffer, MessageBuffer *_requestToLLCMsgBuffer,
    const DynamicStreamId &_rootStreamId)
    : MLCDynamicStream(_configData, _controller, _responseMsgBuffer,
                       _requestToLLCMsgBuffer, false /* isMLCDirect */),
      rootStreamId(_rootStreamId),
      formalParams(_configData->addrGenFormalParams),
      addrGenCallback(_configData->addrGenCallback),
      elementSize(_configData->elementSize),
      isOneIterationBehind(_configData->isOneIterationBehind),
      tailElementIdx(0) {}

void MLCDynamicIndirectStream::receiveStreamData(
    const DynamicStreamSliceId &sliceId, const DataBlock &dataBlock,
    Addr paddrLine) {

  // It is indeed a problem to synchronize the flow control between
  // base stream and indirect stream.
  // It is possible for an indirect stream to receive stream data
  // beyond the tailElementIdx, so we adhoc to fix that.
  // ! This breaks the MaximumNumElement constraint.

  MLC_SLICE_DPRINTF(sliceId, "Receive data vaddr %#x paddr %#x.\n",
                    sliceId.vaddr, paddrLine);

  // Intercept the reduction value.
  if (this->receiveFinalReductionValue(sliceId, dataBlock, paddrLine)) {
    return;
  }

  while (this->tailElementIdx <= sliceId.getStartIdx()) {
    this->allocateSlice();
  }

  assert(sliceId.isValid() && "Invalid stream slice id for stream data.");
  assert(this->dynamicStreamId == sliceId.getDynStreamId() &&
         "Unmatched dynamic stream id.");

  auto numElements = sliceId.getNumElements();
  assert(numElements == 1 && "Can not merge indirect elements.");

  /**
   * It is possible when the core stream engine runs ahead than
   * the LLC stream engine, and the stream data is delivered after
   * the slice is released. In such case we will ignore the
   * stream data.
   *
   * TODO: Properly handle this with sliceIdx.
   */
  if (this->slices.empty()) {
    // The data arrived after the slices is released.
    MLC_SLICE_DPRINTF(sliceId, "Received Data. No Slice and Overflowed? %d.",
                      this->hasOverflowed());
    return;
  } else {
    /**
     * Check if the data is lagging behind. Since we guarantee that
     * all slices will be created during allocation, it is impossible that
     * we may need to insert new slices here.
     */
    auto frontVAddr = this->slices.front().sliceId.vaddr;
    auto frontElementIdx = this->slices.front().sliceId.getStartIdx();
    if (sliceId.getStartIdx() < frontElementIdx) {
      // Definitely behind.
      return;
    } else if (sliceId.getStartIdx() == frontElementIdx) {
      if (!this->isWaitingAck() && sliceId.vaddr < frontVAddr) {
        // Still behind.
        return;
      } else {
        // For stream waiting for Ack, there is no address associated with addr,
        // so we do not check for that.
      }
    }
  }

  /**
   * Find the correct stream slice and insert the data there.
   */
  auto elementSlices = this->findSliceByElementIdx(sliceId.getStartIdx());
  auto slicesBegin = elementSlices.first;
  auto slicesEnd = elementSlices.second;

  auto sliceIter = slicesBegin;
  // We only check for the address if we are not waiting for Ack.
  if (!this->isWaitingAck()) {
    auto targetLineAddr = makeLineAddress(sliceId.vaddr);
    while (sliceIter != slicesEnd) {
      assert(sliceIter->sliceId.getStartIdx() == sliceId.getStartIdx());
      auto sliceLineAddr = makeLineAddress(sliceIter->sliceId.vaddr);
      if (sliceLineAddr == targetLineAddr) {
        break;
      } else if (sliceLineAddr < targetLineAddr) {
        ++sliceIter;
      } else {
        MLC_SLICE_PANIC(sliceId, "Failed to find slice %#x.\n", targetLineAddr);
      }
    }
  }
  if (sliceIter == slicesEnd) {
    MLC_SLICE_PANIC(sliceId, "Failed to find slice.\n");
  }

  if (!sliceIter->dataReady) {
    sliceIter->setData(dataBlock, this->controller->curCycle());
  } else {
    // The only exception is the second Ack for RangeSync.
    if (!(this->shouldRangeSync() &&
          this->getStaticStream()->isAtomicComputeStream())) {
      MLC_SLICE_PANIC(sliceId, "Receive StreamData twice.");
    }
  }
  if (sliceIter->coreStatus == MLCStreamSlice::CoreStatusE::WAIT_DATA) {
    // Sanity check that LLC and Core generated the same address.
    // ! Core is line address.
    if (sliceIter->coreSliceId.vaddr != makeLineAddress(sliceId.vaddr)) {
      MLC_SLICE_PANIC(sliceId, "Mismatch between Core %#x and LLC %#x.\n",
                      sliceIter->coreSliceId.vaddr, sliceId.vaddr);
    }
    this->makeResponse(*sliceIter);
  } else if (sliceIter->coreStatus == MLCStreamSlice::CoreStatusE::WAIT_ACK) {
    // Ack the stream element.
    // TODO: Send the packet back via normal message buffer.
    // hack("Indirect slices acked element %llu size %llu header %llu.\n",
    //      sliceId.getStartIdx(), this->slices.size(),
    //      this->slices.front().sliceId.getStartIdx());
    this->makeAck(*sliceIter);
  } else if (sliceIter->coreStatus == MLCStreamSlice::CoreStatusE::ACK_READY) {
    MLC_SLICE_PANIC(sliceId, "Received multiple acks.");
  }
  this->advanceStream();
}

void MLCDynamicIndirectStream::receiveBaseStreamData(uint64_t elementIdx,
                                                     uint64_t baseData) {

  MLC_S_DPRINTF(this->dynamicStreamId,
                "Receive BaseStreamData element %lu data %lu tailSliceIdx %lu "
                "tailElementIdx %lu.\n",
                elementIdx, baseData, this->tailSliceIdx, this->tailElementIdx);

  // It's possible that we are behind the base stream?
  while (this->tailElementIdx <= elementIdx) {
    this->allocateSlice();
  }

  if (this->isWaitingNothing()) {
    // If we are waiting for nothing, this is the only place we advance.
    MLC_S_DPRINTF(this->dynamicStreamId, "WaitNothing. Skip BaseElement.\n");
    this->advanceStream();
    return;
  }

  if (this->slices.empty()) {
    // We better be overflowed, unless we are Reduction.
    if (!this->getStaticStream()->isReduction()) {
      assert(this->hasOverflowed() && "No slices when not overflowed.");
    }
    return;
  } else {
    if (elementIdx < this->slices.front().sliceId.getStartIdx()) {
      // The stream is lagging behind the core. The slice has already been
      // released.
      return;
    }
  }

  auto elementVAddr = this->genElementVAddr(elementIdx, baseData);
  auto elementSize = this->elementSize;

  DynamicStreamSliceId sliceId;
  sliceId.getDynStreamId() = this->dynamicStreamId;
  sliceId.getStartIdx() = elementIdx;
  sliceId.getEndIdx() = elementIdx + 1;

  // Search for the slices with the elementIdx.
  auto elementSlices = this->findSliceByElementIdx(elementIdx);
  auto slicesBegin = elementSlices.first;
  auto slicesEnd = elementSlices.second;

  // Iterate through elementSlices.
  auto totalSliceSize = 0;
  while (totalSliceSize < elementSize) {
    Addr curSliceVAddr = elementVAddr + totalSliceSize;
    // Make sure the slice is contained within one line.
    int lineOffset = curSliceVAddr % RubySystem::getBlockSizeBytes();
    auto curSliceSize = std::min(
        elementSize - totalSliceSize,
        static_cast<int>(RubySystem::getBlockSizeBytes()) - lineOffset);
    // Here we set the slice vaddr and size.
    sliceId.vaddr = curSliceVAddr;
    sliceId.size = curSliceSize;
    // Find the slice.
    auto sliceIter =
        this->findOrInsertSliceBySliceId(slicesBegin, slicesEnd, sliceId);

    MLC_SLICE_DPRINTF(sliceId,
                      "Processing totalSize %d elementSize %d vaddr %#x.\n",
                      totalSliceSize, elementSize, curSliceVAddr);

    /**
     * The reason we do this is to check if the indirect stream faulted,
     * in which case the LLC won't send data here, and we need to mark
     * this slice Fault and release it.
     */
    auto cpuDelegator = this->getStaticStream()->getCPUDelegator();
    Addr curSlicePAddr;
    if (!cpuDelegator->translateVAddrOracle(curSliceVAddr, curSlicePAddr)) {
      // This slice has faulted.
      sliceIter->coreStatus = MLCStreamSlice::CoreStatusE::FAULTED;
      MLC_SLICE_DPRINTF(sliceIter->sliceId,
                        "Faulted Indirect Element VAddr %#x.\n",
                        sliceIter->sliceId.vaddr);
    }
    totalSliceSize += curSliceSize;
  }

  this->advanceStream();
}

void MLCDynamicIndirectStream::advanceStream() {
  this->tryPopStream();

  /**
   * Do not try to allocate if the LLCElement is not created yet.
   * This is to avoid running ahead than the BaseS, which is in charge of
   * allocating LLCElement.
   */
  auto maxAllocElementIdx = this->tailElementIdx + this->maxNumSlices;
  if (auto llcDynS =
          LLCDynamicStream::getLLCStream(this->getDynamicStreamId())) {
    maxAllocElementIdx =
        std::min(maxAllocElementIdx, llcDynS->getNextInitElementIdx());
  }

  // Of course we need to allocate more slices.
  while (this->tailSliceIdx - this->headSliceIdx < this->maxNumSlices &&
         !this->hasOverflowed()) {
    if (this->tailElementIdx >= maxAllocElementIdx) {
      MLC_S_DPRINTF(this->getDynamicStreamId(),
                    "CanNot Allocate Element %llu, MaxAlloc %llu.\n",
                    this->tailElementIdx, maxAllocElementIdx);
      break;
    }
    this->allocateSlice();
  }

  // We may need to schedule advance stream if the first slice is FAULTED,
  // as no other event will cause it to be released.
  if (!this->slices.empty()) {
    auto frontCoreStatus = this->slices.front().coreStatus;
    if (frontCoreStatus == MLCStreamSlice::CoreStatusE::FAULTED ||
        frontCoreStatus == MLCStreamSlice::CoreStatusE::DONE) {
      this->scheduleAdvanceStream();
    }
  }

  // Let's try to schedule advanceStream for the baseStream.
  this->baseStream->scheduleAdvanceStream();
}

void MLCDynamicIndirectStream::allocateSlice() {
  // For indirect stream, there is no merging, so it's pretty simple
  // to allocate new slice.
  DynamicStreamSliceId sliceId;
  sliceId.getDynStreamId() = this->dynamicStreamId;
  sliceId.getStartIdx() = this->tailElementIdx;
  sliceId.getEndIdx() = this->tailElementIdx + 1;

  MLC_SLICE_DPRINTF(sliceId, "Allocated indirect slice.\n");

  this->slices.emplace_back(sliceId);
  this->stream->statistic.numMLCAllocatedSlice++;

  /**
   * The core still wait for ack from the MergedStoreStream to synchronize with
   * the LLC. So far this is done with hack.
   * It would not issue request for reduction stream.
   */
  if (this->isWaitingAck()) {
    this->slices.back().coreStatus = MLCStreamSlice::CoreStatusE::WAIT_ACK;
  } else if (this->isWaitingNothing()) {
    this->slices.back().coreStatus = MLCStreamSlice::CoreStatusE::DONE;
  }

  this->tailSliceIdx++;
  this->tailElementIdx++;
}

bool MLCDynamicIndirectStream::hasOverflowed() const {
  return this->hasTotalTripCount() &&
         this->tailElementIdx > this->getTotalTripCount();
}

void MLCDynamicIndirectStream::setTotalTripCount(int64_t totalTripCount,
                                                 Addr brokenPAddr) {
  MLC_S_PANIC(this->getDynamicStreamId(), "Set TotalTripCount for IndirectS.");
}

Addr MLCDynamicIndirectStream::genElementVAddr(uint64_t elementIdx,
                                               uint64_t baseData) {

  StreamValue baseValue;
  baseValue.front() = baseData;
  auto getBaseStreamValue = [this,
                             &baseValue](uint64_t streamId) -> StreamValue {
    if (!this->baseStream->getStaticStream()->isCoalescedHere(streamId)) {
      MLC_S_PANIC(this->getDynamicStreamId(), "Invalid BaseStreamId %llu.",
                  streamId);
    }
    return baseValue;
  };

  return this->addrGenCallback
      ->genAddr(elementIdx, this->formalParams, getBaseStreamValue)
      .front();
}

std::pair<MLCDynamicIndirectStream::SliceIter,
          MLCDynamicIndirectStream::SliceIter>
MLCDynamicIndirectStream::findSliceByElementIdx(uint64_t elementIdx) {
  auto ret = std::make_pair<SliceIter, SliceIter>(this->slices.end(),
                                                  this->slices.end());
  // hack("findSliceByElementIdx when slices are %d.\n", this->slices.size());
  for (auto iter = this->slices.begin(), end = this->slices.end(); iter != end;
       ++iter) {
    auto lhsElementIdx = iter->sliceId.getStartIdx();
    if (lhsElementIdx == elementIdx && ret.first == end) {
      // Find lhs.
      ret.first = iter;
    } else if (lhsElementIdx > elementIdx && ret.second == end) {
      // Find rhs.
      ret.second = iter;
      break;
    }
  }

  if (ret.first == this->slices.end()) {
    MLC_S_PANIC(this->getDynamicStreamId(),
                "Failed to find slice for Element %llu.\n", elementIdx);
  }
  return ret;
}

MLCDynamicIndirectStream::SliceIter
MLCDynamicIndirectStream::findOrInsertSliceBySliceId(
    const SliceIter &begin, const SliceIter &end,
    const DynamicStreamSliceId &sliceId) {
  auto ret = begin;
  if (ret->sliceId.vaddr == 0) {
    // This first slice has not been used. directly use it.
    assert(ret->sliceId.getStartIdx() == sliceId.getStartIdx() &&
           "Invalid elementIdx.");
    ret->sliceId.vaddr = sliceId.vaddr;
    return ret;
  }
  auto targetLineAddr = makeLineAddress(sliceId.vaddr);
  while (ret != end) {
    assert(ret->sliceId.getStartIdx() == sliceId.getStartIdx() &&
           "Invalid elementIdx.");
    auto lineAddr = makeLineAddress(ret->sliceId.vaddr);
    if (lineAddr == targetLineAddr) {
      // We found it.
      return ret;
    } else if (lineAddr < targetLineAddr) {
      // Keep searching.
      ++ret;
    } else {
      // This should be our next slice.
      break;
    }
  }
  // Insert one slice before ret.
  ret = this->slices.emplace(ret, sliceId);
  this->tailSliceIdx++;
  this->stream->statistic.numMLCAllocatedSlice++;
  MLC_SLICE_DPRINTF(ret->sliceId, "Insert indirect slice.\n");
  return ret;
}

MLCDynamicStream::SliceIter MLCDynamicIndirectStream::findSliceForCoreRequest(
    const DynamicStreamSliceId &sliceId) {

  if (this->slices.empty()) {
    MLC_S_PANIC(this->dynamicStreamId,
                "No slices for request, overflowed %d, totalTripCount %lu.\n",
                this->hasOverflowed(), this->getTotalTripCount());
  }

  // We try to allocate slices.
  auto elementSlices = this->findSliceByElementIdx(sliceId.getStartIdx());
  return this->findOrInsertSliceBySliceId(elementSlices.first,
                                          elementSlices.second, sliceId);
}

bool MLCDynamicIndirectStream::receiveFinalReductionValue(
    const DynamicStreamSliceId &sliceId, const DataBlock &dataBlock,
    Addr paddrLine) {
  auto S = this->getStaticStream();
  if (!(S->isReduction() || S->isPointerChaseIndVar())) {
    // Only Reduction/PtrChaseIV need final value.
    return false;
  }
  if (!this->isWaitingNothing()) {
    // We are expecting something.
    return false;
  }

  auto dynCoreS = S->getDynamicStream(this->getDynamicStreamId());
  if (!dynCoreS) {
    MLC_SLICE_PANIC(sliceId,
                    "CoreDynS released before receiving FinalReductionValue.");
  }
  if (dynCoreS->finalReductionValueReady) {
    MLC_SLICE_PANIC(sliceId, "FinalReductionValue already ready.");
  }
  auto size = sizeof(dynCoreS->finalReductionValue);
  memcpy(dynCoreS->finalReductionValue.uint8Ptr(), dataBlock.getData(0, size),
         size);
  dynCoreS->finalReductionValueReady = true;
  MLC_SLICE_DPRINTF_(MLCRubyStreamReduce, sliceId, "Notify final reduction.\n");

  return true;
}