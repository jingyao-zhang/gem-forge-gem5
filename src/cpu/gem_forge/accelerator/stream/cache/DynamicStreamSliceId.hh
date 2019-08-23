#ifndef __CPU_TDG_ACCELERATOR_DYNAMIC_STREAM_SLICE_ID_HH__
#define __CPU_TDG_ACCELERATOR_DYNAMIC_STREAM_SLICE_ID_HH__

#include "DynamicStreamId.hh"

struct DynamicStreamSliceId {
  DynamicStreamId streamId;
  uint64_t startIdx;
  uint64_t endIdx;
  // Represent the size of the slice.
  int sizeInByte;

  DynamicStreamSliceId() : streamId(), startIdx(0), endIdx(0), sizeInByte(0) {}

  bool isValid() const { return !(this->startIdx == 0 && this->endIdx == 0); }

  uint64_t getStartIdx() const { return this->startIdx; }
  uint64_t getNumElements() const { return this->endIdx - this->startIdx; }
  int getSizeInByte() const { return this->sizeInByte; }

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