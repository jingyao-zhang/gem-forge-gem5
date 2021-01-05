#ifndef __CPU_TDG_ACCELERATOR_LLC_DYNAMIC_STREAM_H__
#define __CPU_TDG_ACCELERATOR_LLC_DYNAMIC_STREAM_H__

#include "LLCStreamElement.hh"

#include "SlicedDynamicStream.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"
#include "mem/ruby/protocol/CoherenceRequestType.hh"
#include "mem/ruby/system/RubySystem.hh"

#include <list>
#include <map>
#include <set>
#include <vector>

class AbstractStreamAwareController;

/**
 * Represent generated request to LLC bank.
 */
class LLCDynamicStream;
struct LLCStreamRequest {
  LLCStreamRequest(const DynamicStreamSliceId &_sliceId, Addr _paddrLine,
                   CoherenceRequestType _type)
      : sliceId(_sliceId), paddrLine(_paddrLine), requestType(_type) {}
  DynamicStreamSliceId sliceId;
  Addr paddrLine;
  CoherenceRequestType requestType;
  bool translationDone = false;

  // Optional fields.
  DataBlock dataBlock;

  // Optional for StreamStore request.
  int storeSize = 8;

  // Optional for Multicast request, excluding the original stream
  std::vector<DynamicStreamSliceId> multicastSliceIds;

  // Optional for StreamForward request, the receiver stream id.
  DynamicStreamId forwardToStreamId;
};

class LLCDynamicStream {
public:
  ~LLCDynamicStream();

  Stream *getStaticStream() const { return this->configData->stream; }
  uint64_t getStaticId() const { return this->configData->dynamicId.staticId; }
  const DynamicStreamId &getDynamicStreamId() const {
    return this->configData->dynamicId;
  }

  int32_t getMemElementSize() const { return this->configData->elementSize; }
  bool isPointerChase() const { return this->configData->isPointerChase; }
  bool isPseudoOffload() const { return this->configData->isPseudoOffload; }
  bool isOneIterationBehind() const {
    return this->configData->isOneIterationBehind;
  }
  bool isPredicated() const { return this->configData->isPredicated; }
  bool isPredicatedTrue() const {
    assert(this->isPredicated());
    return this->configData->isPredicatedTrue;
  }
  const DynamicStreamId &getPredicateStreamId() const {
    assert(this->isPredicated());
    return this->configData->predicateStreamId;
  }
  bool hasTotalTripCount() const;
  uint64_t getTotalTripCount() const;
  bool hasIndirectDependent() const {
    auto S = this->getStaticStream();
    return !this->indirectStreams.empty() || this->isPointerChase() ||
           (S->isLoadStream() && S->getEnabledStoreFunc());
  }

  void setMulticastGroupLeader(LLCDynamicStream *S) {
    this->multicastGroupLeader = S;
  }
  LLCDynamicStream *getMulticastGroupLeader() {
    return this->multicastGroupLeader;
  }

  Addr peekVAddr() const;
  const DynamicStreamSliceId &peekSlice() const;
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
  uint64_t getNextSliceIdx() const { return this->sliceIdx; }

  void addCredit(uint64_t n);

  DynamicStreamSliceId consumeNextSlice() {
    assert(this->isNextSliceAllocated() && "Next slice is not allocated yet.");
    this->sliceIdx++;
    return this->slicedStream.getNextSlice();
  }

  void
  traceEvent(const ::LLVM::TDG::StreamFloatEvent::StreamFloatEventType &type);

  /**************************************************************************
   * To better managing the life cycle of LLCDynamicStream, instead of
   * allocating them at when StreamConfig hits the LLC SE, we allocate them
   * at once at the MLC SE. And they are released lazily at once when all
   * floating streams are terminated. Therefore, they have states:
   * INITIALIZED: Initialized by MLC SE but before LLC SE receives StreamConfig.
   * RUNNING: The stream is running at one LLC SE.
   * MIGRATING: The stream is migrating to the next LLC SE.
   * TERMINATED: The LLC SE terminated the stream.
   *
   * To correctly handle these, we have a global map from DynamicStreamId to
   * LLCDynamicStream *. We also remember the list of streams that are allocated
   * together, so that we can deallocate them at the same time.
   **************************************************************************/
  enum State {
    INITIALIZED,
    RUNNING,
    MIGRATING,
    TERMINATED,
  };
  static std::string stateToString(State state);

  State getState() const { return this->state; }
  void setState(State state);

  bool isTerminated() const { return this->state == State::TERMINATED; }
  bool isLLCConfigured() const { return this->state != State::INITIALIZED; }

  void configuredLLC(AbstractStreamAwareController *llcController) {
    this->setState(State::RUNNING);
    this->llcController = llcController;
  }

  void migratingStart();
  void migratingDone(AbstractStreamAwareController *llcController);

  void terminate();

  static LLCDynamicStream *getLLCStream(const DynamicStreamId &dynId) {
    if (GlobalLLCDynamicStreamMap.count(dynId)) {
      return GlobalLLCDynamicStreamMap.at(dynId);
    } else {
      return nullptr;
    }
  }
  static LLCDynamicStream *getLLCStreamPanic(const DynamicStreamId &dynId) {
    if (auto S = LLCDynamicStream::getLLCStream(dynId)) {
      return S;
    }
    panic("Failed to get LLCDynamicStream %s.", dynId);
  }
  static void allocateLLCStreams(AbstractStreamAwareController *mlcController,
                                 CacheStreamConfigureVec &configs);

  bool isBasedOn(const DynamicStreamId &baseId) const;
  void recvStreamForward(uint64_t baseElementIdx,
                         const DynamicStreamSliceId &sliceId,
                         const DataBlock &dataBlk);

private:
  State state = INITIALIZED;
  AbstractStreamAwareController *mlcController;
  AbstractStreamAwareController *llcController = nullptr;

  // Private controller as user should use allocateLLCStreams().
  LLCDynamicStream(AbstractStreamAwareController *_mlcController,
                   CacheStreamConfigureDataPtr _configData);

  static std::unordered_map<DynamicStreamId, LLCDynamicStream *,
                            DynamicStreamIdHasher>
      GlobalLLCDynamicStreamMap;
  static std::unordered_map<NodeID, std::list<std::vector<LLCDynamicStream *>>>
      GlobalMLCToLLCDynamicStreamGroupMap;
  static void allocateLLCStream(AbstractStreamAwareController *mlcController,
                                CacheStreamConfigureDataPtr &config);

  Cycles curCycle() const;
  int curLLCBank() const;

  uint64_t nextElementIdx = 0;

public:
  const CacheStreamConfigureDataPtr configData;
  SlicedDynamicStream slicedStream;

  // Remember the last reduction element, avoid auto releasing.
  LLCStreamElementPtr lastReductionElement;

  /**
   * Here we remember the dependent streams.
   * IndirectStreams is just the "UsedBy" dependence.
   */
  std::vector<LLCDynamicStream *> indirectStreams;
  std::vector<CacheStreamConfigureDataPtr> sendToConfigs;

  /**
   * Remember the basic information for BaseOn information.
   * Note: Here we are outside of CacheStreamConfigureData, so we can directly
   * store the shared_ptr without creating circular dependence.
   * Note: We may use streams from remote LLC bank, so here we just remember the
   * config.
   */
  std::vector<CacheStreamConfigureDataPtr> baseOnConfigs;

  // Base stream.
  LLCDynamicStream *baseStream = nullptr;

  // Dependent predicated streams.
  std::unordered_set<LLCDynamicStream *> predicatedStreams;

  // Base predicate stream.
  LLCDynamicStream *predicateStream = nullptr;

  Cycles issueClearCycle = Cycles(4);
  // Configure cycle.
  const Cycles configureCycle;
  // Last issued cycle.
  Cycles prevIssuedCycle = Cycles(0);
  // Last migrate cycle.
  Cycles prevMigrateCycle = Cycles(0);

  /**
   * Used to implement CompactStore.
   * ! Only valid for floating MergedStoreStream.
   */
  Addr prevStorePAddrLine = 0;
  Cycles prevStoreCycle = Cycles(0);

  /**
   * Transient states that should be reset after migration.
   * ! Only valid for DirectStream.
   */
  LLCDynamicStream *multicastGroupLeader = nullptr;

  // Next slice index to be issued.
  uint64_t sliceIdx;
  // For flow control.
  uint64_t allocatedSliceIdx;
  /**
   * Number of requests of this stream (not including indirect streams)
   * issued but data not ready.
   */
  int inflyRequests;

  /**
   * Map from ElementIdx to LLCStreamElement.
   */
  std::map<uint64_t, LLCStreamElementPtr> idxToElementMap;

  /**
   * Indirect elements that has seen the direct stream element's data
   * and is waiting to be issued.
   * Indexed by element idx.
   */
  std::multimap<uint64_t,
                std::pair<LLCDynamicStream *, ConstLLCStreamElementPtr>>
      readyIndirectElements;

  /**
   * The elements that is predicated by this stream.
   */
  std::map<uint64_t,
           std::list<std::pair<LLCDynamicStream *, ConstLLCStreamElementPtr>>>
      waitingPredicatedElements;

  void updateIssueClearCycle();
  bool shouldUpdateIssueClearCycleMemorized = true;
  bool shouldUpdateIssueClearCycleInitialized = false;
  bool shouldUpdateIssueClearCycle();

  /**
   * Sanity check that stream should be correctly terminated.
   */
  void sanityCheckStreamLife();

  /**
   * Allocate the element for myself and all UsedByStream.
   * There may already be an element if multiple slices
   * contain a part of that element.
   * @return bool: whether allocation happened.
   */
  bool allocateElement(uint64_t elementIdx, Addr vaddr);
};

using LLCDynamicStreamPtr = LLCDynamicStream *;

#endif