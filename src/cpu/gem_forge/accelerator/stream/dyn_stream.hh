#ifndef __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__
#define __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__

#include "fifo_entry_idx.hh"
#include "addr_gen_callback.hh"

#include <memory>
#include <vector>

class ThreadContext;

class StreamElement;
class StreamEngine;

/**
 * Holds some information of a dynamic instance of a stream,
 * e.g. callback to generate addresses.
 */
struct DynamicStream {

  const DynamicStreamId dynamicStreamId;
  const uint64_t configSeqNum;
  const Cycles configCycle;
  ThreadContext *tc;
  const FIFOEntryIdx prevFIFOIdx;

  /**
   * Head is the newest element.
   * Tail is the dummy node before the oldest element.
   */
  StreamElement *head;
  StreamElement *stepped;
  StreamElement *tail;
  int allocSize = 0;
  int stepSize = 0;
  FIFOEntryIdx FIFOIdx;

  // Whether the dynamic stream is offloaded to cache.
  bool offloadedToCacheAsRoot = false;
  bool offloadedToCache = false;

  // Whether the StreamConfig has executed (ready to go).
  bool configExecuted = false;

  // Whether the StreamEnd has dispatched (waiting to be released).
  bool endDispatched = false;

  // Address generator.
  DynamicStreamFormalParamV addrGenFormalParams;
  AddrGenCallbackPtr addrGenCallback;

  // Predication compute.
  DynamicStreamFormalParamV predFormalParams;
  ExecFuncPtr predCallback;

  /**
   * Optional constant update value.
   * 1. If the load stream is upgraded to an update stream.
   * 2. If the store stream is merged into load and is constant store.
   */
  uint64_t constUpdateValue = 0;

  /**
   * Optional initial value for reduction stream.
   */
  uint64_t initialValue = 0;

  // Optional total length of this dynamic stream. -1 as indefinite.
  int64_t totalTripCount = -1;

  DynamicStream(const DynamicStreamId &_dynamicStreamId, uint64_t _configSeqNum,
                Cycles _configCycle, ThreadContext *_tc,
                const FIFOEntryIdx &_prevFIFOIdx, StreamEngine *_se);
  DynamicStream(const DynamicStream &other) = delete;
  DynamicStream(DynamicStream &&other) = delete;
  DynamicStream &operator=(const DynamicStream &other) = delete;
  DynamicStream &operator=(DynamicStream &&other) = delete;

  ~DynamicStream();

  Cycles getAvgTurnAroundCycle() const { return this->avgTurnAroundCycle; }
  int getNumLateElement() const { return this->numLateElement; }

  /**
   * Update step cycle.
   */
  void updateReleaseCycle(Cycles releaseCycle, bool late);

  /***********************************************************************
   * API to manage the elements of this stream.
   ***********************************************************************/
  /**
   * Get element with the elementIdx.
   */
  StreamElement *getElementByIdx(uint64_t elementIdx) const;
  /**
   * Get the first unstepped element of the dynamic stream.
   */
  StreamElement *getFirstUnsteppedElement();
  /**
   * Get previous element in the chain of the stream.
   * Notice that it may return the (dummy) element->stream->tail if this is
   * the first element for that stream.
   */
  StreamElement *getPrevElement(StreamElement *element);
  /**
   * Remove one unstepped element from the last dynamic stream.
   */
  StreamElement *releaseElementUnstepped();

  void dump() const;

private:
  /**
   * Used to compute flow control signal.
   */
  constexpr static int HistoryWindowSize = 10;
  Cycles lastReleaseCycle = Cycles(0);
  Cycles avgTurnAroundCycle = Cycles(0);
  uint64_t numReleaseElement = 0;
  int lateElementCount = 0;
  int numLateElement = 0;
};

#endif