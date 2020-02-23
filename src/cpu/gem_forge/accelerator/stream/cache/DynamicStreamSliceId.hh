#ifndef __CPU_TDG_ACCELERATOR_DYNAMIC_STREAM_SLICE_ID_HH__
#define __CPU_TDG_ACCELERATOR_DYNAMIC_STREAM_SLICE_ID_HH__

#include "DynamicStreamId.hh"

#include "base/types.hh"

/**
 * The core stream engine manages stream at granularity of element.
 * However, this is not ideal for cache stream engine, as we want to
 * coalesce continuous elements to the same cache line. Things get
 * more complicated when there is overlapping between elements and
 * one element can span across multiple cache lines.
 *
 * This represent the basic unit how the cache system manages streams.
 * A slice is a piece of continuous memory, and does not span across
 * cache lines. It also remembers elements within this slice,
 * [lhsElementIdx, rhsElementIdx).
 *
 * Notice that one element may span across multiple cache lines, and
 * thus the data in a slice may only be a portion of the whole element.
 */
struct DynamicStreamSliceId {
  DynamicStreamId streamId;
  uint64_t lhsElementIdx;
  uint64_t rhsElementIdx;
  Addr vaddr;
  int size;

  DynamicStreamSliceId()
      : streamId(), lhsElementIdx(0), rhsElementIdx(0), vaddr(0), size(0) {}

  bool isValid() const { return !(this->lhsElementIdx == 0 && this->rhsElementIdx == 0); }
  void clear() {
    this->streamId = DynamicStreamId();
    this->lhsElementIdx = 0;
    this->rhsElementIdx = 0;
    this->vaddr = 0;
    this->size = 0;
  }

  uint64_t getStartIdx() const { return this->lhsElementIdx; }
  uint64_t getNumElements() const { return this->rhsElementIdx - this->lhsElementIdx; }
  int getSize() const { return this->size; }

  bool operator==(const DynamicStreamSliceId &other) const {
    return this->streamId == other.streamId &&
           this->lhsElementIdx == other.lhsElementIdx && this->rhsElementIdx == other.rhsElementIdx;
  }

  bool operator!=(const DynamicStreamSliceId &other) const {
    return !(this->operator==(other));
  }
};

std::ostream &operator<<(std::ostream &os, const DynamicStreamSliceId &slice);

#endif