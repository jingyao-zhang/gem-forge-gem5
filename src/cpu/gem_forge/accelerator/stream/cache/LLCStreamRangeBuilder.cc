#include "LLCStreamRangeBuilder.hh"

#include "debug/StreamRangeSync.hh"
#define DEBUG_TYPE StreamRangeSync
#include "../stream_log.hh"

LLCStreamRangeBuilder::LLCStreamRangeBuilder(LLCDynamicStream *_stream,
                                             int _elementsPerRange,
                                             int64_t _totalTripCount)
    : stream(_stream), elementsPerRange(_elementsPerRange),
      totalTripCount(_totalTripCount) {}

void LLCStreamRangeBuilder::addElementAddress(uint64_t elementIdx, Addr vaddr,
                                              Addr paddr, int size) {
  /**
   * So far this is pretty limited and we enforce these checks:
   * 1. The element can not across multiple pages.
   * 2. Element address is added in order.
   */
  if (elementIdx != this->nextElementIdx) {
    LLC_S_PANIC(
        this->stream->getDynamicStreamId(),
        "[RangeBuilder] Element not added in order: expect %llu got %llu.",
        this->nextElementIdx, elementIdx);
  }
  if (vaddr == 0 || paddr == 0) {
    LLC_S_PANIC(this->stream->getDynamicStreamId(),
                "[RangeBuilder] Invalid element %llu vaddr %#x paddr %#x.",
                elementIdx, vaddr, paddr);
  }
  const Addr PageSize = 4096;
  if (((vaddr + size - 1) / PageSize) != (vaddr / PageSize)) {
    LLC_S_PANIC(this->stream->getDynamicStreamId(),
                "[RangeBuilder] Element across pages: vaddr %#x, size %d.",
                vaddr, size);
  }
  if (this->totalTripCount != -1 && elementIdx >= this->totalTripCount) {
    LLC_S_PANIC(this->stream->getDynamicStreamId(),
                "[RangeBuilder] ElementIdx overflow, total %llu.",
                this->totalTripCount);
  }
  this->vaddrRange.add(vaddr, vaddr + size);
  this->paddrRange.add(paddr, paddr + size);
  this->nextElementIdx++;
  this->tryBuildRange();
}

bool LLCStreamRangeBuilder::hasReadyRanges() const {
  if (this->readyRanges.empty()) {
    return false;
  }
  // Recursively check all indirect streams.
  for (auto dynIS : this->stream->getIndStreams()) {
    if (dynIS->shouldRangeSync() &&
        !dynIS->getRangeBuilder()->hasReadyRanges()) {
      return false;
    }
  }
  return true;
}

DynamicStreamAddressRangePtr LLCStreamRangeBuilder::popReadyRange() {
  auto range = this->readyRanges.front();
  this->readyRanges.pop_front();
  // Recursively merge all indirect streams' range.
  for (auto dynIS : this->stream->getIndStreams()) {
    if (dynIS->shouldRangeSync()) {
      auto indRange = dynIS->getRangeBuilder()->popReadyRange();
      range->addRange(indRange);
    }
  }
  return range;
}

void LLCStreamRangeBuilder::tryBuildRange() {
  if ((this->totalTripCount != -1 &&
       this->nextElementIdx == this->totalTripCount) ||
      ((this->nextElementIdx % this->elementsPerRange) == 0)) {
    // Time to build another range.
    DynamicStreamElementRangeId elementRange;
    elementRange.streamId = this->stream->getDynamicStreamId();
    elementRange.lhsElementIdx = this->prevBuiltElementIdx;
    elementRange.rhsElementIdx = this->nextElementIdx;
    auto range = std::make_shared<DynamicStreamAddressRange>(
        elementRange, this->vaddrRange, this->paddrRange);
    LLC_S_DPRINTF(this->stream->getDynamicStreamId(),
                  "[RangeBuilder] Built %s.\n", *range);
    this->readyRanges.push_back(range);
    this->prevBuiltElementIdx = this->nextElementIdx;
    this->vaddrRange.clear();
    this->paddrRange.clear();
  }
}