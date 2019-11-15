#ifndef __GEM_FORGE_ACCELERATOR_STREAM_FIFO_ENTRY_IDX_HH__
#define __GEM_FORGE_ACCELERATOR_STREAM_FIFO_ENTRY_IDX_HH__

#include "cache/DynamicStreamSliceId.hh"
#include "cpu/gem_forge/llvm_insts.hh"

struct FIFOEntryIdx {
  DynamicStreamId streamId;
  uint64_t configSeqNum;
  uint64_t entryIdx;
  FIFOEntryIdx()
      : streamId(), configSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
        entryIdx(0) {}
  FIFOEntryIdx(const DynamicStreamId &_streamId)
      : streamId(_streamId), configSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
        entryIdx(0) {}
  FIFOEntryIdx(const DynamicStreamId &_streamId, uint64_t _configSeqNum)
      : streamId(_streamId), configSeqNum(_configSeqNum), entryIdx(0) {}
  void next() { this->entryIdx++; }
  void prev() { this->entryIdx--; }
  void newInstance(uint64_t configSeqNum) {
    this->entryIdx = 0;
    this->streamId.streamInstance++;
    this->configSeqNum = configSeqNum;
  }

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
};

#endif