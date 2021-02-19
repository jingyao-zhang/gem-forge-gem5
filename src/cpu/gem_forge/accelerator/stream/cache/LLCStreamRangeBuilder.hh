#ifndef __CPU_GEM_FORGE_ACCELERATOR_LLC_STREAM_RANGE_BUILDER_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_LLC_STREAM_RANGE_BUILDER_HH__

#include "DynamicStreamAddressRange.hh"
#include "LLCDynamicStream.hh"

#include <list>

class LLCStreamRangeBuilder {
public:
  LLCStreamRangeBuilder(LLCDynamicStream *_stream, int _elementsPerRange,
                        int64_t _totalTripCount);

  void addElementAddress(uint64_t elementIdx, Addr vaddr, Addr paddr, int size);

  bool hasReadyRanges() const { return !this->readyRanges.empty(); }
  DynamicStreamAddressRangePtr popReadyRange() {
    auto range = this->readyRanges.front();
    this->readyRanges.pop_front();
    return range;
  }

private:
  LLCDynamicStream *stream;
  const int elementsPerRange;
  const int64_t totalTripCount;
  uint64_t nextElementIdx = 0;
  uint64_t prevBuiltElementIdx = 0;

  AddressRange vaddrRange;
  AddressRange paddrRange;
  std::list<DynamicStreamAddressRangePtr> readyRanges;

  int curLLCBank() const { return this->stream->curLLCBank(); }
};

#endif