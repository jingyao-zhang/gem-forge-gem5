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
 * [startIdx, endIdx).
 *
 * Notice that one element may span across multiple cache lines, and
 * thus the data in a slice may only be a portion of the whole element.
 */
struct DynamicStreamSliceId {
  DynamicStreamId streamId;
  uint64_t startIdx;
  uint64_t endIdx;
  Addr vaddr;
  int size;

  DynamicStreamSliceId()
      : streamId(), startIdx(0), endIdx(0), vaddr(0), size(0) {}

  bool isValid() const { return !(this->startIdx == 0 && this->endIdx == 0); }

  uint64_t getStartIdx() const { return this->startIdx; }
  uint64_t getNumElements() const { return this->endIdx - this->startIdx; }
  int getSize() const { return this->size; }

  bool operator==(const DynamicStreamSliceId &other) const {
    return this->streamId == other.streamId &&
           this->startIdx == other.startIdx && this->endIdx == other.endIdx;
  }

  bool operator!=(const DynamicStreamSliceId &other) const {
    return !(this->operator==(other));
  }
};

std::ostream &operator<<(std::ostream &os, const DynamicStreamSliceId &slice);

#endif