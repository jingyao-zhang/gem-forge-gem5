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
      isOneIterationBehind(_configData->isOneIterationBehind),
      totalTripCount(_configData->totalTripCount) {
  if (this->isOneIterationBehind) {
    // This indirect stream is behind one iteration, which means that the first
    // element is not handled by LLC stream. The stream buffer should start at
    // the second element. We simply release the first element here.
    assert(!this->slices.empty() && "No initial slices.");
    // Let's do some sanity check.
    auto &firstSliceId = this->slices.front().sliceId;
    assert(firstSliceId.lhsElementIdx == 0 && "Start index should always be 0.");
    assert(firstSliceId.rhsElementIdx - firstSliceId.lhsElementIdx == 1 &&
           "Indirect stream should never merge slices.");
    MLC_SLICE_DPRINTF(firstSliceId, "Initial offset pop.\n");
    this->headSliceIdx++;
    this->slices.pop_front();
  }
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
   * Here we reversely search for it to save time.
   */
  for (auto slice = this->slices.rbegin(), end = this->slices.rend();
       slice != end; ++slice) {
    if (slice->sliceId.lhsElementIdx == sliceId.lhsElementIdx) {
      // Found the slice.
      if (slice->sliceId.getNumElements() != numElements) {
        MLC_S_PANIC("Mismatch numElements, incoming %d, slice %d.\n",
                    numElements, slice->sliceId.getNumElements());
      }
      /**
       * ! Notice that for indirect stream, we also have to set the vaddr.
       * ! So that it knows how to construct the response packet.
       */
      slice->sliceId.vaddr = sliceId.vaddr;
      slice->setData(msg.m_DataBlk);
      if (slice->coreStatus == MLCStreamSlice::CoreStatusE::WAIT) {
        // Sanity check that LLC and Core generated the same address.
        // ! Core is line address.
        if (slice->coreSliceId.vaddr != makeLineAddress(sliceId.vaddr)) {
          MLC_SLICE_PANIC(sliceId, "Mismatch between Core %#x and LLC %#x.\n",
                          slice->coreSliceId.vaddr, sliceId.vaddr);
        }
        this->makeResponse(*slice);
      }
      this->advanceStream();
      return;
    }
  }

  MLC_SLICE_PANIC(sliceId, "Fail to find the slice. Tail %lu.\n",
                  this->tailSliceIdx);
}

void MLCDynamicIndirectStream::advanceStream() {
  // We simply pop the stream and rely on other API to allocate slices.
  this->popStream();
  // Of course we need to allocate more slices.
  while (this->tailSliceIdx - this->headSliceIdx < this->maxNumSlices &&
         !this->hasOverflowed()) {
    this->allocateSlice();
  }
}

void MLCDynamicIndirectStream::allocateSlice() {
  // For indirect stream, there is no merging, so it's pretty simple
  // to allocate new slice.
  DynamicStreamSliceId sliceId;
  sliceId.lhsElementIdx = this->tailSliceIdx;
  sliceId.rhsElementIdx = this->tailSliceIdx + 1;

  MLC_SLICE_DPRINTF(sliceId, "Allocated indirect slice.\n");

  this->slices.emplace_back(sliceId);
  this->stream->statistic.numMLCAllocatedSlice++;

  this->tailSliceIdx++;
}

bool MLCDynamicIndirectStream::hasOverflowed() const {
  return this->totalTripCount > 0 &&
         (this->tailSliceIdx >= (this->totalTripCount + 1));
}

int64_t MLCDynamicIndirectStream::getTotalTripCount() const {
  return this->totalTripCount;
}