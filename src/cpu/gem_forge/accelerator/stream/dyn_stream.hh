#ifndef __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__
#define __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__

#include "addr_gen_callback.hh"
#include "cache/DynStreamAddressRange.hh"
#include "cache/StreamFloatPlan.hh"
#include "fifo_entry_idx.hh"
#include "stream_inner_loop_dep.hh"

#include <array>
#include <memory>
#include <vector>

namespace gem5 {

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

  constexpr static InstSeqNum InvalidInstSeqNum = 0;

  Stream *stream;
  StreamEngine *se;
  const DynStreamId dynStreamId;
  const InstSeqNum configSeqNum;
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

  /**
   * How many elements to step each time. By default this is just 1.
   * Used to optimize for LoopElminated InnerReduction, where we only need
   * the LastElement of each InnerMostLoop.
   */
  int64_t stepElemCount = 1;

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
  uint64_t getFirstFloatElemIdxOfStepGroup() const;
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

  bool isElemFloatedToCache(uint64_t elemIdx) const {
    return this->isFloatedToCache() &&
           this->getAdjustedFirstFloatElemIdx() <= elemIdx;
  }

  bool isLoopElimInCoreStoreCmpS() const;

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
  void updateFloatInfoForElems();

  StreamFloatPlan &getFloatPlan() { return this->floatPlan; }
  const StreamFloatPlan &getFloatPlan() const { return this->floatPlan; }

  void dispatchStreamEnd(uint64_t seqNum);
  void rewindStreamEnd();
  void commitStreamEnd();

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
  ExecFuncWithFormalParamV predCallbacks;

  // Store value compute.
  DynStreamFormalParamV storeFormalParams;
  ExecFuncPtr storeCallback;
  DynStreamFormalParamV loadFormalParams;
  ExecFuncPtr loadCallback;

  /**
   * Optional initial/final value for reduction stream.
   */
  StreamValue initialValue;
  std::map<uint64_t, StreamValue> innerFinalValueMap;
  void setInnerFinalValue(uint64_t elemIdx, const StreamValue &value);
  bool isInnerFinalValueReady(uint64_t elemIdx) const;
  const StreamValue &getInnerFinalValue(uint64_t elemIdx) const;

  /**
   * Whether this DynS is used for nest RemoteConfig.
   */
  bool depRemoteNestRegion = false;
  bool hasDepRemoteNestRegion() const { return this->depRemoteNestRegion; }
  void setDepRemoteNestRegion(bool depRemoteNestRegion) {
    this->depRemoteNestRegion = depRemoteNestRegion;
  }

  // Optional total length of this dynamic stream. -1 as indefinite.
  static constexpr int64_t InvalidTripCount = -1;
  bool hasTotalTripCount() const {
    return this->totalTripCount != InvalidTripCount;
  }
  int64_t getTotalTripCount() const { return this->totalTripCount; }
  void setTotalAndInnerTripCount(int64_t tripCount);
  bool hasZeroTripCount() const {
    return this->hasTotalTripCount() && this->getTotalTripCount() == 0;
  }
  bool hasInnerTripCount() const {
    return this->innerTripCount != InvalidTripCount;
  }
  int64_t getInnerTripCount() const { return this->innerTripCount; }
  void setInnerTripCount(int64_t innerTripCount);

  /**
   * Return true if the DynStream has known trip count and this is the second
   * element of the InnerMostLoop, e.g., Elem N + 1.
   */
  bool isInnerSecondElem(uint64_t elemIdx) const;

  /**
   * Return true if the DynStream has known trip count and this is the last
   * element of the InnerMostLoop.
   */
  bool isInnerLastElem(uint64_t elemIdx) const;

  /**
   * Return true if the DynStream has known total trip count and this is the
   * second last element of the InnerMostLoop.
   */
  bool isInnerSecondLastElem(uint64_t elemIdx) const;

  /**
   * Compute the number of bytes per element, also considering overlapping
   * for linear streams.
   */
  int32_t getBytesPerMemElement() const;

  DynStreamEdges baseEdges;
  DynStreamEdges backDepEdges;
  void addBaseDynStreams();
  void addOuterDepDynStreams(StreamEngine *outerSE, InstSeqNum outerSeqNum);

  /**
   * Get the reuse/skip count on a BaseS.
   */
  int getBaseElemReuseCount(Stream *baseS) const;
  int getBaseElemSkipCount(Stream *baseS) const;

  std::list<DynStream *> stepDynStreams;
  void addStepStreams();

  /**
   * Record and manage dynamic InnerLoopBaseStream.
   * Unlike normal StreamDepEdge, here each OuterLoopStream element depends on
   * one INSTANCE of InnerLoopStream.
   */
  StreamInnerLoopDepTracker innerLoopDepTracker;

  /**
   * Compute reuse of the base stream element.
   * This is further split into two cases:
   * 1. Base streams from the same loop.
   * 2. Base streams from outer loops.
   */
  void configureBaseDynStreamReuse();
  void configureBaseDynStreamReuseSameLoop(DynStreamDepEdge &edge,
                                           DynStream &baseDynS);
  void configureBaseDynStreamReuseOuterLoop(DynStreamDepEdge &edge,
                                            DynStream &baseDynS);
  void configureBaseDynStreamSkipInnerLoop(DynStreamDepEdge &edge,
                                           DynStream &baseDynS);

  /**
   * Check if base elements of the next allocating element is ready.
   */
  bool areNextBaseElementsAllocated() const;
  bool isNextAddrBaseElementAllocated(const DynStreamDepEdge &edge) const;
  bool isNextValueBaseElementAllocated(const DynStreamDepEdge &edge) const;
  bool isNextBackBaseElementAllocated(const DynStreamDepEdge &edge) const;
  bool areNextBackDepElementsReady(StreamElement *element) const;

  /**
   * Add address base elements to new element.
   * This is further split into two cases:
   * 1. Base streams from edges.
   * 2. Self dependence for reduction stream.
   */
  void addBaseElements(StreamElement *newElement);
  void addAddrBaseElementEdge(StreamElement *newElement,
                              const DynStreamDepEdge &edge);
  void addValueBaseElementEdge(StreamElement *newElement,
                               const DynStreamDepEdge &edge);
  void addBackBaseElementEdge(StreamElement *newElement,
                              const DynStreamDepEdge &edge);

  void tryAddInnerLoopBaseElements(StreamElement *elem);
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
  StreamElement *getElemByIdx(uint64_t elementIdx) const;
  /**
   * Get the first element of the dynamic stream.
   */
  StreamElement *getFirstElem();
  const StreamElement *getFirstElem() const;
  /**
   * Get the first unstepped element of the dynamic stream.
   */
  StreamElement *getFirstUnsteppedElem() const;
  /**
   * Get previous element in the chain of the stream.
   * Notice that it may return the (dummy) element->dynS->tail if this is
   * the first element for that stream.
   */
  StreamElement *getPrevElement(StreamElement *element);
  /**
   * Check if the last dynamic stream has an unstepped element.
   */
  bool hasUnsteppedElem() const;
  /**
   * Check if an element is already stepped.
   * NOTE: The element may even already be released.
   */
  bool isElemStepped(uint64_t elemIdx) const;
  /**
   * Check if an element is released.
   */
  bool isElemReleased(uint64_t elemIdx) const;
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

  void setElementRemoteBank(uint64_t elemIdx, int remoteBank);

  void dump() const;
  std::string dumpString() const;

private:
  /**
   * Remember the total trip count.
   * May be set by StreamLoopBound if has data-dependent loop bound.
   */
  int64_t totalTripCount = InvalidTripCount;
  /**
   * Remember the InnerMost trip count.
   * By default this is the same as TotalTripCount, but is different when the
   * stream configured at some outer loop.
   * Set by Stepper.
   */
  int64_t innerTripCount = InvalidTripCount;

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

public:
  /**
   * Accessor to the NumInnerLoopDepS. Redirect to InnerLoopDepTracker.
   */
  using InnerLoopDepDynIdVec = StreamInnerLoopDepTracker::InnerLoopDepDynIdVec;
  int getNumInnerLoopDepS() const {
    return this->innerLoopDepTracker.getNumInnerLoopDepS();
  }
  const InnerLoopDepDynIdVec &getInnerLoopDepS() const {
    return this->innerLoopDepTracker.getInnerLoopDepS();
  }
  void popInnerLoopDepS(const DynStreamId &dynId) {
    this->innerLoopDepTracker.popInnerLoopDepS(dynId);
  }
  void tryPopInnerLoopDepS(const DynStreamId &dynId) {
    this->innerLoopDepTracker.tryPopInnerLoopDepS(dynId);
  }

private:
  std::unordered_map<uint64_t, int> futureElemBanks;

  void tryCancelFloat();
  void cancelFloat();
};

} // namespace gem5

#endif