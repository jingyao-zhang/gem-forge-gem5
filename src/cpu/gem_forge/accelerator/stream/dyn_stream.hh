#ifndef __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__
#define __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__

#include "addr_gen_callback.hh"
#include "cache/DynamicStreamAddressRange.hh"
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

  using StaticId = DynamicStreamId::StaticId;
  using InstanceId = DynamicStreamId::InstanceId;

  Stream *stream;
  const DynamicStreamId dynamicStreamId;
  const uint64_t configSeqNum;
  const Cycles configCycle;
  ThreadContext *tc;

  DynamicStream(Stream *_stream, const DynamicStreamId &_dynamicStreamId,
                uint64_t _configSeqNum, Cycles _configCycle, ThreadContext *_tc,
                StreamEngine *_se);
  DynamicStream(const DynamicStream &other) = delete;
  DynamicStream(DynamicStream &&other) = delete;
  DynamicStream &operator=(const DynamicStream &other) = delete;
  DynamicStream &operator=(DynamicStream &&other) = delete;

  ~DynamicStream();

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
  std::set<uint64_t> cacheAckedElements;
  /**
   * Similar to StreamAck messages, this remembers the StreamDone messages
   * from the cache. Since StreamDone messages are guaranteed in-order, we
   * just remember the last done ElementIdx.
   * TODO: This could eventually be merged with StreamAck messages.
   */
  uint64_t nextCacheDoneElementIdx = 0;

  // Whether the floating config is delayed until config committed.
  bool offloadConfigDelayed = false;

  // Whether the dynamic stream is offloaded to cache.
  bool offloadedToCacheAsRoot = false;
  bool offloadedToCache = false;
  bool offloadedWithDependent = false;
  bool pseudoOffloadedToCache = false;

  // Whether the dynamic stream is offloaded as fine-grained near-data
  // computing.
  bool offloadedAsNDC = false;
  bool offloadedAsNDCForward = false;

  // Whether the StreamConfig has executed (ready to go).
  bool configExecuted = false;

  // Whether the StreamConfig has committed (no rewind).
  bool configCommitted = false;

  // Whether the StreamEnd has dispatched (waiting to be released).
  InstSeqNum endSeqNum = 0;
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
  DynamicStreamFormalParamV loadFormalParams;
  ExecFuncPtr loadCallback;

  /**
   * Optional initial/final value for reduction stream.
   */
  StreamValue initialValue;
  StreamValue finalReductionValue;
  bool finalReductionValueReady = false;

  // Optional total length of this dynamic stream. -1 as indefinite.
  int64_t totalTripCount = -1;
  bool hasTotalTripCount() const { return this->totalTripCount != -1; }
  int64_t getTotalTripCount() const { return this->totalTripCount; }

  /**
   * Compute the number of bytes per element, also considering overlapping
   * for linear streams.
   */
  int32_t getBytesPerMemElement() const;

  /**
   * This remembers the dynamic dependence between streams.
   * Similar to the static StreamDepEdge, but with more information
   * to correctly align dependences.
   * Additional information:
   * 1. FromInstanceId to get the correct dynamic stream.
   * 2. Alignment to the base element index.
   * 3. Reuse count of the base element.
   */
  struct StreamDepEdge {
    const StaticId baseStaticId = DynamicStreamId::InvalidStaticStreamId;
    const InstanceId baseInstanceId = DynamicStreamId::InvalidInstanceId;
    const StaticId depStaticId = DynamicStreamId::InvalidStaticStreamId;
    const uint64_t alignBaseElement = 0;
    uint64_t reuseBaseElement = 0;
    StreamDepEdge(StaticId _baseStaticId, InstanceId _baseInstanceId,
                  StaticId _depStaticId, uint64_t _alignBaseElement,
                  uint64_t _reuseBaseElement)
        : baseStaticId(_baseStaticId), baseInstanceId(_baseInstanceId),
          depStaticId(_depStaticId), alignBaseElement(_alignBaseElement),
          reuseBaseElement(_reuseBaseElement) {}
  };
  using StreamEdges = std::vector<StreamDepEdge>;
  StreamEdges addrBaseEdges;
  StreamEdges valueBaseEdges;
  StreamEdges backBaseEdges;
  void addBaseDynStreams();
  void addAddrBaseDynStreams();
  void addValueBaseDynStreams();
  void addBackBaseDynStreams();
  /**
   * Compute reuse of the base stream element.
   * This is further split into two cases:
   * 1. Base streams from the same loop.
   * 2. Base streams from outer loops.
   */
  void configureAddrBaseDynStreamReuse();
  void configureAddrBaseDynStreamReuseSameLoop(StreamDepEdge &edge,
                                               DynamicStream &baseDynS);
  void configureAddrBaseDynStreamReuseOuterLoop(StreamDepEdge &edge,
                                                DynamicStream &baseDynS);
  /**
   * Add address base elements to new element.
   * This is further split into two cases:
   * 1. Base streams from edges.
   * 2. Self dependence for reduction stream.
   */
  bool areNextBaseElementsAllocated() const;
  bool areNextAddrBaseElementsAllocated() const;
  bool areNextBackBaseElementsAllocated() const;
  bool areNextValueBaseElementsAllocated() const;
  void addAddrBaseElements(StreamElement *newElement);
  void addAddrBaseElementEdge(StreamElement *newElement,
                              const StreamDepEdge &edge);
  /**
   * Add value base elements for stream computation.
   */
  void addValueBaseElements(StreamElement *newElement);

  /**
   * Should the CoreSE try to issue for the data.
   */
  bool shouldCoreSEIssue() const;

  /**
   * Should we perform range-sync on this stream.
   */
  bool shouldRangeSync() const;

  Cycles getAvgTurnAroundCycle() const { return this->avgTurnAroundCycle; }
  int getNumLateElement() const { return this->numLateElement; }

  /**
   * Update step cycle.
   */
  void updateStatsOnReleaseStepElement(Cycles releaseCycle, uint64_t vaddr,
                                       bool late);

  /***********************************************************************
   * API to manage the elements of this stream.
   ***********************************************************************/
  /**
   * Get element with the elementIdx.
   */
  StreamElement *getElementByIdx(uint64_t elementIdx) const;
  /**
   * Get the first element of the dynamic stream.
   */
  StreamElement *getFirstElement();
  const StreamElement *getFirstElement() const;
  /**
   * Get the first unstepped element of the dynamic stream.
   */
  StreamElement *getFirstUnsteppedElement();
  /**
   * Get previous element in the chain of the stream.
   * Notice that it may return the (dummy) element->dynS->tail if this is
   * the first element for that stream.
   */
  StreamElement *getPrevElement(StreamElement *element);
  /**
   * Add one element to this DynamicStream.
   */
  void allocateElement(StreamElement *newElement);
  // /**
  //  * Withdrawn one element from this DynamicStream.
  //  */
  // void withdrawElement(StreamElement *element);
  /**
   * Remove one unstepped element from the last dynamic stream.
   */
  StreamElement *releaseElementUnstepped();
  /**
   * Remove one stepped element from the first dynamic stream.
   * @param isEnd: This element is stepped by StreamEnd, not StreamStep.
   */
  StreamElement *releaseElementStepped(bool isEnd);

  uint64_t getNumReleasedElements() const { return this->numReleaseElement; }
  uint64_t getStartVAddr() const { return this->startVAddr; }
  uint64_t getNumIssuedRequests() const { return this->numIssuedRequests; }
  void incrementNumIssuedRequests() { this->numIssuedRequests++; }

  void recordHitHistory(bool hitPrivateCache);
  int getTotalHitPrivateCache() const { return this->totalHitPrivateCache; }
  int getHitPrivateCacheHistoryWindowSize() const {
    return HitHistoryWindowSize;
  }

  /**
   * API for range-based stream synchronization.
   */
  void receiveStreamRange(const DynamicStreamAddressRangePtr &range);
  DynamicStreamAddressRangePtr getNextReceivedRange() const;
  void popReceivedRange();
  DynamicStreamAddressRangePtr getCurrentWorkingRange() const {
    return this->currentWorkingRange;
  }
  void setCurrentWorkingRange(DynamicStreamAddressRangePtr ptr) {
    this->currentWorkingRange = ptr;
  }

  void dump() const;
  std::string dumpString() const;

private:
  /**
   * Some statistics for this stream.
   * Notice that here numReleaseElements is measured in SteppedElements.
   */
  uint64_t numReleaseElement = 0;
  uint64_t numIssuedRequests = 0;
  uint64_t startVAddr = 0;

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

  /**
   * This remember the received StreamRange for synchronization.
   * We also remember the current working range, which is actually
   * managed by StreamRangeSyncController.
   */
  std::list<DynamicStreamAddressRangePtr> receivedRanges;
  DynamicStreamAddressRangePtr currentWorkingRange = nullptr;

  void tryCancelFloat();
  void cancelFloat();
};

#endif