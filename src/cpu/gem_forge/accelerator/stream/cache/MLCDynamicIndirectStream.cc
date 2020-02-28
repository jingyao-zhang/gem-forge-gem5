#include "MLCDynamicIndirectStream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStream.hh"

#define DEBUG_TYPE MLCRubyStream
#include "../stream_log.hh"

MLCDynamicIndirectStream::MLCDynamicIndirectStream(
    CacheStreamConfigureData *_configData,
    AbstractStreamAwareController *_controller,
    MessageBuffer *_responseMsgBuffer, MessageBuffer *_requestToLLCMsgBuffer,
    const DynamicStreamId &_rootStreamId)
    : MLCDynamicStream(_configData, _controller, _responseMsgBuffer,
                       _requestToLLCMsgBuffer),
      rootStreamId(_rootStreamId),
      formalParams(_configData->addrGenFormalParams),
      addrGenCallback(_configData->addrGenCallback),
      elementSize(_configData->elementSize),
      isOneIterationBehind(_configData->isOneIterationBehind),
      totalTripCount(_configData->totalTripCount) {
  if (this->isOneIterationBehind) {
    // TODO: Clean this up, as we no longer allocate slices in the constructor.
    // So far I just hack to ignore this case for reduction stream, which is the
    // only case we will have OneIterationBehind stream.
    if (this->stream->isReduction()) {
      return;
    }
    // This indirect stream is behind one iteration, which means that the first
    // element is not handled by LLC stream. The stream buffer should start at
    // the second element. We simply release the first element here.
    assert(!this->slices.empty() && "No initial slices.");
    // Let's do some sanity check.
    auto &firstSliceId = this->slices.front().sliceId;
    assert(firstSliceId.lhsElementIdx == 0 &&
           "Start index should always be 0.");
    assert(firstSliceId.rhsElementIdx - firstSliceId.lhsElementIdx == 1 &&
           "Indirect stream should never merge slices.");
    MLC_SLICE_DPRINTF(firstSliceId, "Initial offset pop.\n");
    this->headSliceIdx++;
    this->slices.pop_front();
  }
  assert(!isOneIterationBehind && "Temporarily disable this.");
}

void MLCDynamicIndirectStream::receiveStreamData(const ResponseMsg &msg) {

  // It is indeed a problem to synchronize the flow control between
  // base stream and indirect stream.
  // It is possible for an indirect stream to receive stream data
  // beyond the tailSliceIdx, so we adhoc to fix that.
  // ! This breaks the MaximumNumElement constraint.

  const auto &sliceId = msg.m_sliceId;
  MLC_SLICE_DPRINTF(sliceId, "Receive data vaddr %#x paddr %#x.\n",
                    sliceId.vaddr, msg.getaddr());

  while (this->tailSliceIdx <= sliceId.lhsElementIdx) {
    this->allocateSlice();
  }

  assert(sliceId.isValid() && "Invalid stream slice id for stream data.");
  assert(this->dynamicStreamId == sliceId.streamId &&
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
    // We better be overflowed.
    assert(this->hasOverflowed() && "No slices when not overflowed.");
  } else {
    if (sliceId.lhsElementIdx < this->slices.front().sliceId.lhsElementIdx) {
      // The stream data is lagging behind. The slice is already
      // released.
      return;
    }
  }

  /**
   * Find the correct stream slice and insert the data there.
   */
  auto elementSlices = this->findSliceByElementIdx(sliceId.lhsElementIdx);
  auto slicesBegin = elementSlices.first;
  auto slicesEnd = elementSlices.second;
  auto sliceIter =
      this->findOrInsertSliceBySliceId(slicesBegin, slicesEnd, sliceId);

  sliceIter->setData(msg.m_DataBlk, this->controller->curCycle());
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
    //      sliceId.lhsElementIdx, this->slices.size(),
    //      this->slices.front().sliceId.lhsElementIdx);
    this->makeAck(*sliceIter);
  }
  this->advanceStream();
}

void MLCDynamicIndirectStream::receiveBaseStreamData(uint64_t elementIdx,
                                                     uint64_t baseData) {

  MLC_S_DPRINTF("Receive BaseStreamData element %lu data %lu.\n", elementIdx,
                baseData);

  // It's possible that we are behind the base stream?
  while (this->tailSliceIdx <= elementIdx) {
    this->allocateSlice();
  }

  if (this->slices.empty()) {
    // We better be overflowed.
    assert(this->hasOverflowed() && "No slices when not overflowed.");
    return;
  } else {
    if (elementIdx < this->slices.front().sliceId.lhsElementIdx) {
      // The stream is lagging behind the core. The slice has already been
      // released.
      return;
    }
  }

  auto elementVAddr = this->genElementVAddr(elementIdx, baseData);
  auto elementSize = this->elementSize;

  DynamicStreamSliceId sliceId;
  sliceId.streamId = this->dynamicStreamId;
  sliceId.lhsElementIdx = elementIdx;
  sliceId.rhsElementIdx = elementIdx + 1;

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
}

void MLCDynamicIndirectStream::allocateSlice() {
  // For indirect stream, there is no merging, so it's pretty simple
  // to allocate new slice.
  DynamicStreamSliceId sliceId;
  sliceId.streamId = this->dynamicStreamId;
  sliceId.lhsElementIdx = this->tailSliceIdx;
  sliceId.rhsElementIdx = this->tailSliceIdx + 1;

  MLC_SLICE_DPRINTF(sliceId, "Allocated indirect slice.\n");

  this->slices.emplace_back(sliceId);
  this->stream->statistic.numMLCAllocatedSlice++;

  /**
   * The core still wait for ack from the MergedStoreStream to synchronize with
   * the LLC. So far this is done with hack.
   * It would not issue request for reduction stream.
   */
  if (this->stream->isMerged() && this->stream->getStreamType() == "store") {
    this->slices.back().coreStatus = MLCStreamSlice::CoreStatusE::WAIT_ACK;
  } else if (this->stream->isReduction()) {
    this->slices.back().coreStatus = MLCStreamSlice::CoreStatusE::DONE;
  }

  this->tailSliceIdx++;
}

bool MLCDynamicIndirectStream::hasOverflowed() const {
  return this->totalTripCount > 0 &&
         (this->tailSliceIdx >= (this->totalTripCount + 1));
}

int64_t MLCDynamicIndirectStream::getTotalTripCount() const {
  return this->totalTripCount;
}

Addr MLCDynamicIndirectStream::genElementVAddr(uint64_t elementIdx,
                                               uint64_t baseData) {
  auto getBaseStreamValue = [baseData](uint64_t baseStreamId) -> uint64_t {
    return baseData;
  };
  return this->addrGenCallback->genAddr(elementIdx, this->formalParams,
                                        getBaseStreamValue);
}

std::pair<MLCDynamicIndirectStream::SliceIter,
          MLCDynamicIndirectStream::SliceIter>
MLCDynamicIndirectStream::findSliceByElementIdx(uint64_t elementIdx) {
  auto ret = std::make_pair<SliceIter, SliceIter>(this->slices.end(),
                                                  this->slices.end());

  for (auto iter = this->slices.begin(), end = this->slices.end(); iter != end;
       ++iter) {
    auto lhsElementIdx = iter->sliceId.lhsElementIdx;
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
    this->panicDump();
  }
  assert(ret.first != this->slices.end() &&
         "Failed to find slices by elementIdx.");
  return ret;
}

MLCDynamicIndirectStream::SliceIter
MLCDynamicIndirectStream::findOrInsertSliceBySliceId(
    const SliceIter &begin, const SliceIter &end,
    const DynamicStreamSliceId &sliceId) {
  auto ret = begin;
  if (ret->sliceId.vaddr == 0) {
    // This first slice has not been used. directly use it.
    assert(ret->sliceId.lhsElementIdx == sliceId.lhsElementIdx &&
           "Invalid elementIdx.");
    ret->sliceId.vaddr = sliceId.vaddr;
    return ret;
  }
  auto targetLineAddr = makeLineAddress(sliceId.vaddr);
  while (ret != end) {
    assert(ret->sliceId.lhsElementIdx == sliceId.lhsElementIdx &&
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
  this->stream->statistic.numMLCAllocatedSlice++;
  MLC_SLICE_DPRINTF(ret->sliceId, "Insert indirect slice.\n");
  return ret;
}

MLCDynamicStream::SliceIter MLCDynamicIndirectStream::findSliceForCoreRequest(
    const DynamicStreamSliceId &sliceId) {

  if (this->slices.empty()) {
    MLC_S_PANIC("No slices for request, overflowed %d, totalTripCount %lu.\n",
                this->hasOverflowed(), this->getTotalTripCount());
  }

  // We try to allocate slices.
  auto elementSlices = this->findSliceByElementIdx(sliceId.lhsElementIdx);
  return this->findOrInsertSliceBySliceId(elementSlices.first,
                                          elementSlices.second, sliceId);
}