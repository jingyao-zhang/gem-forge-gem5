#ifndef __CPU_TDG_ACCELERATOR_LLC_DYNAMIC_STREAM_H__
#define __CPU_TDG_ACCELERATOR_LLC_DYNAMIC_STREAM_H__

#include "SlicedDynamicStream.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"
#include "mem/ruby/system/RubySystem.hh"

#include <list>
#include <map>
#include <set>

class AbstractStreamAwareController;

class LLCDynamicStream {
public:
  LLCDynamicStream(AbstractStreamAwareController *_controller,
                   CacheStreamConfigureData *_configData);
  ~LLCDynamicStream();

  Stream *getStaticStream() { return this->configData.stream; }
  uint64_t getStaticId() const { return this->configData.dynamicId.staticId; }
  const DynamicStreamId &getDynamicStreamId() const {
    return this->configData.dynamicId;
  }

  int32_t getElementSize() const { return this->configData.elementSize; }
  bool isPointerChase() const { return this->configData.isPointerChase; }
  bool isOneIterationBehind() const {
    return this->configData.isOneIterationBehind;
  }

  Addr peekVAddr();
  Addr getVAddr(uint64_t sliceIdx) const;
  bool translateToPAddr(Addr vaddr, Addr &paddr) const;

  /**
   * Check if the next element is allocated in the upper cache level's stream
   * buffer.
   * Used for flow control.
   */
  bool isNextSliceAllocated() const {
    return this->sliceIdx < this->allocatedSliceIdx;
  }

  void addCredit(uint64_t n);

  DynamicStreamSliceId consumeNextSlice() {
    assert(this->isNextSliceAllocated() && "Next slice is not allocated yet.");
    this->sliceIdx++;
    return this->slicedStream.getNextSlice();
  }

  AbstractStreamAwareController *controller;
  const CacheStreamConfigureData configData;
  SlicedDynamicStream slicedStream;
  uint64_t reductionValue = 0;

  // Dependent indirect streams.
  std::list<LLCDynamicStream *> indirectStreams;

  /**
   * Maximum number of issued requests of the base stream that are waiting for
   * the data.
   */
  int maxWaitingDataBaseRequests;

  Cycles issueClearCycle = Cycles(4);
  // Last issued cycle.
  Cycles prevIssuedCycle = Cycles(0);

  // Next slice index to be issued.
  uint64_t sliceIdx;
  // For flow control.
  uint64_t allocatedSliceIdx;
  /**
   * Number of requests of the base stream (not including indirect streams)
   * issued but data not ready.
   * Notice that this number only tracks requests in the current LLC bank and
   * will be cleared when migrating.
   */
  int waitingDataBaseRequests;

  /**
   * Indirect elements that has been waiting for
   * the direct stream element's data.
   * Indexed by element idx.
   */
  std::set<uint64_t> waitingIndirectElements;

  /**
   * Indirect elements that has seen the direct stream element's data
   * and is waiting to be issued.
   * Indexed by element idx.
   */
  std::multimap<uint64_t, std::pair<LLCDynamicStream *, uint64_t>>
      readyIndirectElements;

  void updateIssueClearCycle();
};

using LLCDynamicStreamPtr = LLCDynamicStream *;

#endif