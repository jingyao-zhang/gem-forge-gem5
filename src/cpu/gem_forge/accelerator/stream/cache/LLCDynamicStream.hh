#ifndef __CPU_TDG_ACCELERATOR_LLC_DYNAMIC_STREAM_H__
#define __CPU_TDG_ACCELERATOR_LLC_DYNAMIC_STREAM_H__

#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include <list>
#include <map>
#include <set>

class LLCDynamicStream {
public:
  LLCDynamicStream(CacheStreamConfigureData *_configData);
  ~LLCDynamicStream();

  Stream *getStaticStream() { return this->configData.stream; }
  uint64_t getStaticId() { return this->configData.dynamicId.staticId; }
  int32_t getElementSize() const { return this->configData.elementSize; }

  Addr peekVAddr() const;
  Addr getVAddr(uint64_t idx) const;
  Addr translateToPAddr(Addr vaddr) const;

  /**
   * Check if the next element is allocated in the upper cache level's stream
   * buffer.
   * Used for flow control.
   */
  bool isNextElementAllcoated() const { return this->idx < this->allocatedIdx; }

  void addCredit(uint64_t n);

  uint64_t consumeNextElement() {
    assert(this->isNextElementAllcoated() &&
           "Next element is not allocated yet.");
    return this->idx++;
  }

  const CacheStreamConfigureData configData;
  // Dependent indirect streams.
  std::list<LLCDynamicStream *> indirectStreams;

  // Next element index to be issued.
  uint64_t idx;
  // For flow control.
  uint64_t allocatedIdx;

  /**
   * Indirect elements that has been waiting for
   * the direct stream element's data.
   */
  std::set<uint64_t> waitingIndirectElements;

  /**
   * Indirect elements that has seen the direct stream element's data
   * and is waiting to be issued.
   */
  std::multimap<uint64_t, LLCDynamicStream *> readyIndirectElements;
};

using LLCDynamicStreamPtr = LLCDynamicStream *;

#endif