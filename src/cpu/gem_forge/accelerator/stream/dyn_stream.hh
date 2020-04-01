#ifndef __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__
#define __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__

#include "addr_gen_callback.hh"
#include "fifo_entry_idx.hh"

#include <array>
#include <memory>
#include <vector>

class ThreadContext;

class StreamElement;
class StreamEngine;
class Stream;

/**
 * Holds some information of a dynamic instance of a stream,
 * e.g. callback to generate addresses.
 */
struct DynamicStream {

  Stream *stream;
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
  // A hack to store how many elements has the cache acked.
  uint64_t cacheAcked = 0;
  std::set<uint64_t> cacheAckedElements;

  // Whether the dynamic stream is offloaded to cache.
  bool offloadedToCacheAsRoot = false;
  bool offloadedToCache = false;
  bool offloadedWithDependent = false;
  bool pseudoOffloadedToCache = false;

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

  // Store value compute.
  DynamicStreamFormalParamV storeFormalParams;
  ExecFuncPtr storeCallback;

  /**
   * Optional constant update value.
   * 1. If the load stream is upgraded to an update stream.
   * 2. If the store stream is merged into load and is constant store.
   */
  uint64_t constUpdateValue = 0;

  /**
   * Optional initial/final value for reduction stream.
   */
  uint64_t initialValue = 0;
  uint64_t finalReductionValue = 0;
  bool finalReductionValueReady = false;

  // Optional total length of this dynamic stream. -1 as indefinite.
  int64_t totalTripCount = -1;
  bool hasTotalTripCount() const { return this->totalTripCount != -1; }
  int64_t getTotalTripCount() const { return this->totalTripCount; }

  DynamicStream(Stream *_stream, const DynamicStreamId &_dynamicStreamId,
                uint64_t _configSeqNum, Cycles _configCycle, ThreadContext *_tc,
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

  uint64_t getNumReleasedElements() const { return this->numReleaseElement; }
  uint64_t getNumIssuedRequests() const { return this->numIssuedRequests; }
  void incrementNumIssuedRequests() { this->numIssuedRequests++; }

  void recordHitHistory(bool hitPrivateCache);
  int getTotalHitPrivateCache() const { return this->totalHitPrivateCache; }
  int getHitPrivateCacheHistoryWindowSize() const {
    return HitHistoryWindowSize;
  }

  void dump() const;

private:
  /**
   * Some statistics for this stream.
   */
  uint64_t numReleaseElement = 0;
  uint64_t numIssuedRequests = 0;

  /**
   * Used to compute flow control signal.
   */
  constexpr static int HistoryWindowSize = 10;
  Cycles lastReleaseCycle = Cycles(0);
  Cycles avgTurnAroundCycle = Cycles(0);
  int lateElementCount = 0;
  int numLateElement = 0;

  /**
   * Used to estimate last few request hit statistics.
   * True if hit in private cache.
   */
  constexpr static int HitHistoryWindowSize = 8;
  std::array<bool, HitHistoryWindowSize> hitPrivateCacheHistory;
  int currentHitPrivateCacheHistoryIdx = 0;
  int totalHitPrivateCache = 0;

  void tryCancelFloat();
  void cancelFloat();
};

#endif