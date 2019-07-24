#ifndef __CPU_TDG_ACCELERATOR_LLC_DYNAMIC_STREAM_H__
#define __CPU_TDG_ACCELERATOR_LLC_DYNAMIC_STREAM_H__

#include "cpu/gem_forge/accelerator/stream/stream.hh"

class LLCDynamicStream {
public:
  LLCDynamicStream(CacheStreamConfigureData *_configData);

  Stream *getStaticStream() { return this->configData.stream; }

  Addr peekVAddr() const;
  Addr translateToPAddr(Addr vaddr) const;

  /**
   * Check if the next element is allocated in the upper cache level's stream
   * buffer.
   * Used for flow control.
   */
  bool isNextElementAllcoated() const {
    return this->idx + 1 < this->allocated;
  }

  void consumeNextElement() {
    assert(this->isNextElementAllcoated() &&
           "Next element is not allocated yet.");
    this->idx++;
  }

  const CacheStreamConfigureData configData;
  uint64_t idx;
  // For flow control.
  uint64_t allocated;
};

using LLCDynamicStreamPtr = LLCDynamicStream *;

#endif