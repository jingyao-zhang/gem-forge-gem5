#include "SlicedDynamicStream.hh"
#include "debug/SlicedDynamicStream.hh"
#include "mem/ruby/common/Address.hh"
#include "mem/ruby/system/RubySystem.hh"

#define DEBUG_TYPE SlicedDynamicStream
#include "../stream_log.hh"

SlicedDynamicStream::SlicedDynamicStream(CacheStreamConfigureData *_configData)
    : streamId(_configData->dynamicId), formalParams(_configData->formalParams),
      addrGenCallback(_configData->addrGenCallback),
      elementSize(_configData->elementSize), tailIdx(0) {}

DynamicStreamSliceId SlicedDynamicStream::getNextSlice() {
  while (slices.empty() || slices.front().endIdx == this->tailIdx) {
    // Allocate until it's guaranteed that the first slice has no more
    // overlaps.
    this->allocateOneElement();
  }
  auto slice = this->slices.front();
  this->slices.pop_front();
  return slice;
}

const DynamicStreamSliceId &SlicedDynamicStream::peekNextSlice() {
  while (slices.empty() || slices.front().endIdx == this->tailIdx) {
    // Allocate until it's guaranteed that the first slice has no more
    // overlaps.
    this->allocateOneElement();
  }
  return this->slices.front();
}

void SlicedDynamicStream::allocateOneElement() {

  // Let's not worry about indirect streams here.
  auto lhs = this->addrGenCallback->genAddr(this->tailIdx, this->formalParams,
                                            getStreamValueFail);
  auto rhs = lhs + this->elementSize;

  // Break to cache line granularity, [lhsBlock, rhsBlock]
  auto lhsBlock = makeLineAddress(lhs);
  auto rhsBlock = makeLineAddress(rhs - 1);

  DYN_S_DPRINTF(this->streamId, "Allocate element %llu, block [%#x, %#x].\n",
                this->tailIdx, lhsBlock, rhsBlock);

  // Update existing slices to the new element if there is overlap.
  auto curBlock = lhsBlock;
  for (auto &slice : this->slices) {
    if (slice.vaddr == curBlock) {
      assert(slice.endIdx == tailIdx && "Hole in overlapping elements.");
      slice.endIdx++;
      curBlock += RubySystem::getBlockSizeBytes();
      if (curBlock > rhsBlock) {
        // We are done.
        break;
      }
    }
  }

  // Insert new slices.
  while (curBlock <= rhsBlock) {
    // So far we can only handle monotonic increasing address.
    if (!this->slices.empty()) {
      assert(this->slices.back().vaddr < curBlock &&
             "We can only slice monotonic increasing stream.");
    }
    this->slices.emplace_back();
    auto &slice = this->slices.back();
    slice.streamId = this->streamId;
    slice.startIdx = this->tailIdx;
    slice.endIdx = this->tailIdx + 1;
    slice.vaddr = curBlock;
    slice.size = RubySystem::getBlockSizeBytes();
    curBlock += RubySystem::getBlockSizeBytes();
  }

  this->tailIdx++;
}