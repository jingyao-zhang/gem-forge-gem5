#ifndef __GEM_FORGE_SLICED_DYNAMIC_STREAM_H__
#define __GEM_FORGE_SLICED_DYNAMIC_STREAM_H__

/**
 * This will slice the stream into slices. A slice is a piece of the stream,
 * and can span across multiple elements (when continuous elements are
 * coalesced) or be sub-element (when multiple streams are coalesced into a
 * wider stream).
 */

#include "CacheStreamConfigureData.hh"
#include "DynamicStreamSliceId.hh"

#include <deque>

class SlicedDynamicStream {
public:
  SlicedDynamicStream(CacheStreamConfigureData *_configData,
                      bool _coalesceContinuousElements);

  DynamicStreamSliceId getNextSlice();
  const DynamicStreamSliceId &peekNextSlice() const;

  /**
   * Check if we have allocated beyond the end of the stream.
   * Instead of terminating the stream, here I take a "soft"
   * approach to ease the implementation complexicity.
   *
   * Notice that we allow (totalTripCount + 1) elements as
   * StreamEnd will consume one element and we have to be synchronized
   * with the core's StreamEngine.
   */
  bool hasOverflowed() const {
    return this->hasOverflowed(this->peekNextSlice().lhsElementIdx);
  }

  int64_t getTotalTripCount() const { return this->totalTripCount; }

  /**
   * Helper function to get element vaddr and size.
   */
  Addr getElementVAddr(uint64_t elementIdx) const;
  int32_t getElementSize() const { return this->elementSize; }
  float getElementPerSlice() const { return this->elementPerSlice; }

private:
  // TODO: Move this out of SlicedDynamicStream and make it only
  // TODO: worry about slicing.
  DynamicStreamId streamId;
  DynamicStreamFormalParamV formalParams;
  AddrGenCallbackPtr addrGenCallback;
  int32_t elementSize;
  // On average how many elements per slice.
  float elementPerSlice = 1.0f;
  /**
   * -1 means indefinite.
   */
  const int64_t totalTripCount;

  const bool coalesceContinuousElements;

  /**
   * Internal states.
   * ! Evil trick to make peekNextSlice constant.
   */
  mutable uint64_t tailElementIdx;
  /**
   * The headIdx that can be checked for slicing.
   */
  mutable uint64_t sliceHeadElementIdx;
  mutable std::deque<DynamicStreamSliceId> slices;

  void allocateOneElement() const;
  bool hasOverflowed(uint64_t elementIdx) const {
    return this->totalTripCount > 0 && elementIdx >= (this->totalTripCount + 1);
  }
};

#endif
