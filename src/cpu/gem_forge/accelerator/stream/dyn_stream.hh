#ifndef __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__
#define __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__

#include "addr_gen_callback.hh"
#include "cache/DynStreamAddressRange.hh"
#include "cache/StreamFloatPlan.hh"
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
struct DynStream {

  using StaticId = DynStreamId::StaticId;
  using InstanceId = DynStreamId::InstanceId;

  Stream *stream;
  StreamEngine *se;
  const DynStreamId dynStreamId;
  const uint64_t configSeqNum;
  const Cycles configCycle;
  ThreadContext *tc;

  DynStream(Stream *_stream, const DynStreamId &_dynStreamId,
            uint64_t _configSeqNum, Cycles _configCycle, ThreadContext *_tc,
            StreamEngine *_se);
  DynStream(const DynStream &other) = delete;
  DynStream(DynStream &&other) = delete;
  DynStream &operator=(const DynStream &other) = delete;
  DynStream &operator=(DynStream &&other) = delete;

  ~DynStream();

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
   * Offload flags are now set to private.
   */
  bool isFloatConfigDelayed() const { return this->floatConfigDelayed; }
  bool isFloatedToCacheAsRoot() const { return this->floatedToCacheAsRoot; }
  bool isFloatedToCache() const { return this->floatedToCache; }
  bool isFloatedWithDependent() const { return this->floatedWithDependent; }
  bool isFloatedAsNDC() const { return this->floatedAsNDC; }
  bool isFloatedAsNDCForward() const { return this->floatedAsNDCForward; }
  bool isPseudoFloatedToCache() const { return this->pseudoFloatedToCache; }
  uint64_t getFirstFloatElemIdx() const {
    return this->floatPlan.getFirstFloatElementIdx();
  }
  uint64_t getAdjustedFirstFloatElemIdx() const {
    auto firstFloatElemIdx = this->getFirstFloatElemIdx();
    return this->floatedOneIterBehind ? (firstFloatElemIdx + 1)
                                      : (firstFloatElemIdx);
  }
  uint64_t getNextCacheDoneElemIdx() const {
    return this->nextCacheDoneElementIdx;
  }

  // Compute the number of floated element until a given ElementIdx.
  uint64_t getNumFloatedElemUntil(uint64_t untilElemIdx) const {
    auto firstFloatElemIdx = this->getFirstFloatElemIdx();
    if (firstFloatElemIdx > untilElemIdx) {
      return 0;
    } else {
      return untilElemIdx - firstFloatElemIdx;
    }
  }

  void setFloatConfigDelayed(bool val) { this->floatConfigDelayed = val; }
  void setFloatedToCacheAsRoot(bool val) { this->floatedToCacheAsRoot = val; }
  void setFloatedToCache(bool val) { this->floatedToCache = val; }
  void setFloatedWithDependent(bool val) { this->floatedWithDependent = val; }
  void setFloatedAsNDC(bool val) { this->floatedAsNDC = val; }
  void setFloatedAsNDCForward(bool val) { this->floatedAsNDCForward = val; }
  void setPseudoFloatedToCache(bool val) { this->pseudoFloatedToCache = val; }
  void setFloatedOneIterBehind(bool val) { this->floatedOneIterBehind = val; }
  void setFirstFloatElemIdx(uint64_t val) {
    this->floatPlan.delayFloatUntil(val);
    this->nextCacheDoneElementIdx = val;
  }
  void setNextCacheDoneElemIdx(uint64_t val) {
    this->nextCacheDoneElementIdx = val;
  }

  StreamFloatPlan &getFloatPlan() { return this->floatPlan; }
  const StreamFloatPlan &getFloatPlan() const { return this->floatPlan; }

private:
  // Whether the floating config is delayed until config committed.
  bool floatConfigDelayed = false;

  // Whether the dynamic stream is floated to cache.
  bool floatedToCacheAsRoot = false;
  bool floatedToCache = false;
  StreamFloatPlan floatPlan;
  bool floatedWithDependent = false;
  bool pseudoFloatedToCache = false;

  // Whether the dynamic stream is floated as fine-grained near-data
  // computing.
  bool floatedAsNDC = false;
  bool floatedAsNDCForward = false;

  // Whether this stream is floated as one iteration behind.
  bool floatedOneIterBehind = false;

  /**
   * Similar to StreamAck messages, this remembers the StreamDone messages
   * from the cache. Since StreamDone messages are guaranteed in-order, we
   * just remember the last done ElementIdx.
   * NOTE: This should start with FirstFloatedElemIdx.
   * TODO: This could eventually be merged with StreamAck messages.
   */
  uint64_t nextCacheDoneElementIdx = 0;

public:
  // Whether the StreamConfig has executed (ready to go).
  bool configExecuted = false;

  // Whether the StreamConfig has committed (no rewind).
  bool configCommitted = false;

  // Whether the StreamEnd has dispatched (waiting to be released).
  InstSeqNum endSeqNum = 0;
  bool endDispatched = false;

  // Address generator.
  DynStreamFormalParamV addrGenFormalParams;
  AddrGenCallbackPtr addrGenCallback;

  // Predication compute.
  DynStreamFormalParamV predFormalParams;
  ExecFuncPtr predCallback;

  // Store value compute.
  DynStreamFormalParamV storeFormalParams;
  ExecFuncPtr storeCallback;
  DynStreamFormalParamV loadFormalParams;
  ExecFuncPtr loadCallback;

  /**
   * Optional initial/final value for reduction stream.
   */
  StreamValue initialValue;
  StreamValue finalReductionValue;
  bool finalReductionValueReady = false;

  // Optional total length of this dynamic stream. -1 as indefinite.
  static constexpr int64_t InvalidTotalTripCount = -1;
  bool hasTotalTripCount() const {
    return this->totalTripCount != InvalidTotalTripCount;
  }
  int64_t getTotalTripCount() const { return this->totalTripCount; }
  void setTotalTripCount(int64_t totalTripCount);
  bool hasZeroTripCount() const {
    return this->hasTotalTripCount() && this->getTotalTripCount() == 0;
  }

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
    enum TypeE { Addr, Value, Back };
    static const char *typeToString(const TypeE &type);
    const TypeE type = Addr;
    const StaticId baseStaticId = DynStreamId::InvalidStaticStreamId;
    const InstanceId baseInstanceId = DynStreamId::InvalidInstanceId;
    const StaticId depStaticId = DynStreamId::InvalidStaticStreamId;
    const uint64_t alignBaseElement = 0;
    uint64_t reuseBaseElement = 1;
    StreamDepEdge(TypeE _type, StaticId _baseStaticId,
                  InstanceId _baseInstanceId, StaticId _depStaticId,
                  uint64_t _alignBaseElement, uint64_t _reuseBaseElement)
        : type(_type), baseStaticId(_baseStaticId),
          baseInstanceId(_baseInstanceId), depStaticId(_depStaticId),
          alignBaseElement(_alignBaseElement),
          reuseBaseElement(_reuseBaseElement) {}
    bool isAddrEdge() const { return this->type == TypeE::Addr; }
    bool isValueEdge() const { return this->type == TypeE::Value; }
    bool isBackEdge() const { return this->type == TypeE::Back; }
  };
  using StreamEdges = std::vector<StreamDepEdge>;
  StreamEdges baseEdges;
  StreamEdges backDepEdges;
  void addBaseDynStreams();

  /**
   * Get the reuse count on a BaseS.
   */
  int getBaseElemReuseCount(Stream *baseS) const;

  std::list<DynStream *> stepDynStreams;
  void addStepStreams();

  /**
   * Record and manage dynamic InnerLoopBaseStream.
   * Unlike normal StreamDepEdge, here each OuterLoopStream element depends on
   * one INSTANCE of InnerLoopStream.
   */
  using InnerLoopBaseDynStreamMapT = std::map<StaticId, StreamEdges>;
  InnerLoopBaseDynStreamMapT innerLoopBaseEdges;
  StreamEdges &getInnerLoopBaseEdges(StaticId baseStaticId);
  const StreamEdges &getInnerLoopBaseEdges(StaticId baseStaticId) const;
  void pushInnerLoopBaseDynStream(StreamDepEdge::TypeE type,
                                  StaticId baseStaticId,
                                  InstanceId baseInstanceId,
                                  StaticId depStaticId);

  /**
   * Compute reuse of the base stream element.
   * This is further split into two cases:
   * 1. Base streams from the same loop.
   * 2. Base streams from outer loops.
   */
  void configureBaseDynStreamReuse();
  void configureBaseDynStreamReuseSameLoop(StreamDepEdge &edge,
                                           DynStream &baseDynS);
  void configureBaseDynStreamReuseOuterLoop(StreamDepEdge &edge,
                                            DynStream &baseDynS);

  /**
   * Check if base elements of the next allocating element is ready.
   */
  bool areNextBaseElementsAllocated() const;
  bool isNextAddrBaseElementAllocated(const StreamDepEdge &edge) const;
  bool isNextValueBaseElementAllocated(const StreamDepEdge &edge) const;
  bool isNextBackBaseElementAllocated(const StreamDepEdge &edge) const;
  bool areNextBackDepElementsReady(StreamElement *element) const;

  /**
   * Add address base elements to new element.
   * This is further split into two cases:
   * 1. Base streams from edges.
   * 2. Self dependence for reduction stream.
   */
  void addBaseElements(StreamElement *newElement);
  void addAddrBaseElementEdge(StreamElement *newElement,
                              const StreamDepEdge &edge);
  void addValueBaseElementEdge(StreamElement *newElement,
                               const StreamDepEdge &edge);
  void addBackBaseElementEdge(StreamElement *newElement,
                              const StreamDepEdge &edge);

  void tryAddInnerLoopBaseElements(StreamElement *elem);
  bool areInnerLoopBaseElementsAllocated(StreamElement *elem) const;
  void addInnerLoopBaseElements(StreamElement *elem);

  /**
   * Should the CoreSE try to issue for the data.
   */
  bool shouldCoreSEIssue() const;

  /**
   * Does the CoreSE need the address.
   * This is used to skip fetching A[i] for B[A[i]].
   */
  bool coreSENeedAddress() const;

  /**
   * Should this element get oracle value.
   * Adhoc implement the feature to skip fetching A[i] for B[A[i]].
   */
  bool coreSEOracleValueReady() const;

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
   * Check if the last dynamic stream has an unstepped element.
   */
  bool hasUnsteppedElement();
  /**
   * Step one element.
   */
  StreamElement *stepElement(bool isEnd);
  /**
   * Unstep one element.
   */
  StreamElement *unstepElement();

  /**
   * Add one element to this DynStream.
   */
  void allocateElement(StreamElement *newElement);
  // /**
  //  * Withdrawn one element from this DynStream.
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
  void receiveStreamRange(const DynStreamAddressRangePtr &range);
  DynStreamAddressRangePtr getNextReceivedRange() const;
  void popReceivedRange();
  DynStreamAddressRangePtr getCurrentWorkingRange() const {
    return this->currentWorkingRange;
  }
  void setCurrentWorkingRange(DynStreamAddressRangePtr ptr) {
    this->currentWorkingRange = ptr;
  }

  void dump() const;
  std::string dumpString() const;

private:
  /**
   * Remember the total trip count.
   * May be set by StreamLoopBound if has data-dependent loop bound.
   */
  int64_t totalTripCount = InvalidTotalTripCount;

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
  std::list<DynStreamAddressRangePtr> receivedRanges;
  DynStreamAddressRangePtr currentWorkingRange = nullptr;

  void tryCancelFloat();
  void cancelFloat();
};

std::ostream &operator<<(std::ostream &os,
                         const DynStream::StreamDepEdge::TypeE &type);
std::string to_string(const DynStream::StreamDepEdge::TypeE &type);

#endif