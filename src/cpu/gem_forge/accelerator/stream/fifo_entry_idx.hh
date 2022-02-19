#ifndef __GEM_FORGE_ACCELERATOR_STREAM_FIFO_ENTRY_IDX_HH__
#define __GEM_FORGE_ACCELERATOR_STREAM_FIFO_ENTRY_IDX_HH__

#include "cache/DynStreamSliceId.hh"
#include "cpu/gem_forge/llvm_insts.hh"

#include <iostream>

struct FIFOEntryIdx {
  DynStreamId streamId;
  uint64_t entryIdx;
  FIFOEntryIdx() : streamId(), entryIdx(0) {}
  FIFOEntryIdx(const DynStreamId &_streamId)
      : streamId(_streamId), entryIdx(0) {}
  FIFOEntryIdx(const DynStreamId &_streamId, uint64_t _entryIdx)
      : streamId(_streamId), entryIdx(_entryIdx) {}
  void next() { this->entryIdx++; }
  void prev() { this->entryIdx--; }

  bool operator==(const FIFOEntryIdx &other) const {
    return this->streamId == other.streamId && this->entryIdx == other.entryIdx;
  }
  bool operator!=(const FIFOEntryIdx &other) const {
    return !(this->operator==(other));
  }
  bool operator>(const FIFOEntryIdx &other) const {
    return this->streamId.streamInstance > other.streamId.streamInstance ||
           (this->streamId.streamInstance == other.streamId.streamInstance &&
            this->entryIdx > other.entryIdx);
  }
  friend std::ostream &operator<<(std::ostream &os, const FIFOEntryIdx &id) {
    return os << id.streamId << id.entryIdx << '-';
  }
};

struct FIFOEntryIdxHasher {
  std::size_t operator()(const FIFOEntryIdx &key) const {
    return (DynStreamIdHasher()(key.streamId)) ^
           std::hash<uint64_t>()(key.entryIdx);
  }
};

#endif