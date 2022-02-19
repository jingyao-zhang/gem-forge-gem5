#ifndef __CPU_GEM_FORGE_LLC_DYN_STREAM_H__
#define __CPU_GEM_FORGE_LLC_DYN_STREAM_H__

#include "LLCStreamElement.hh"

#include "SlicedDynStream.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"
#include "mem/ruby/protocol/CoherenceRequestType.hh"
#include "mem/ruby/system/RubySystem.hh"

#include <deque>
#include <list>
#include <map>
#include <set>
#include <vector>

class AbstractStreamAwareController;
class LLCStreamRangeBuilder;
class LLCStreamEngine;
class LLCStreamCommitController;

/**
 * Represent generated request to LLC bank.
 */
class LLCDynStream;
using LLCDynStreamPtr = LLCDynStream *;

struct LLCStreamRequest {
  LLCStreamRequest(Stream *_S, const DynStreamSliceId &_sliceId,
                   Addr _paddrLine, MachineType _destMachineType,
                   CoherenceRequestType _type)
      : S(_S), sliceId(_sliceId), paddrLine(_paddrLine),
        destMachineType(_destMachineType), requestType(_type) {}
  Stream *S;
  DynStreamSliceId sliceId;
  Addr paddrLine;
  MachineType destMachineType;
  CoherenceRequestType requestType;
  bool translationDone = false;

  // Optional fields.
  DataBlock dataBlock;

  // Optional for StreamStore request.
  int storeSize = 8;

  // Optional for StreamForward request with smaller payload size.
  int payloadSize = RubySystem::getBlockSizeBytes();

  // Optional for Multicast request, excluding the original stream
  std::vector<DynStreamSliceId> multicastSliceIds;

  // Optional for StreamForward request, the receiver stream id.
  DynStreamId forwardToStreamId;
};

class LLCDynStream {
public:
  friend class LLCStreamRangeBuilder;
  friend class LLCStreamCommitController;

  ~LLCDynStream();

  AbstractStreamAwareController *getMLCController() const {
    return this->mlcController;
  }

  Stream *getStaticS() const { return this->configData->stream; }
  DynStream *getCoreDynStream() const {
    return this->getStaticS()->getDynStream(this->getDynStreamId());
  }
  uint64_t getStaticId() const { return this->configData->dynamicId.staticId; }
  const DynStreamId &getDynStreamId() const {
    return this->configData->dynamicId;
  }

  int32_t getMemElementSize() const { return this->configData->elementSize; }
  bool isPointerChase() const { return this->configData->isPointerChase; }
  bool isPseudoOffload() const { return this->configData->isPseudoOffload; }
  bool isOneIterationBehind() const {
    return this->configData->isOneIterationBehind;
  }
  bool isIndirect() const { return this->baseStream != nullptr; }
  bool isIndirectReduction() const {
    return this->isIndirect() && this->baseStream->isIndirect() &&
           this->getStaticS()->isReduction();
  }
  bool shouldRangeSync() const { return this->configData->rangeSync; }
  bool isPredicated() const { return this->configData->isPredicated; }
  bool isPredicatedTrue() const {
    assert(this->isPredicated());
    return this->configData->isPredicatedTrue;
  }
  const DynStreamId &getPredicateStreamId() const {
    assert(this->isPredicated());
    return this->configData->predicateStreamId;
  }

  /**
   * We must query the sliced stream for total trip count.
   */
  bool hasTotalTripCount() const;
  int64_t getTotalTripCount() const;
  void setTotalTripCount(int64_t totalTripCount);

  /**
   * Query the offloaded machine type.
   */
  MachineType getFloatMachineTypeAtElem(uint64_t elementIdx) const;

  bool hasIndirectDependent() const {
    auto S = this->getStaticS();
    return !this->getIndStreams().empty() || this->isPointerChase() ||
           (S->isLoadStream() && S->getEnabledStoreFunc());
  }

  void setMulticastGroupLeader(LLCDynStream *S) {
    this->multicastGroupLeader = S;
  }
  LLCDynStream *getMulticastGroupLeader() { return this->multicastGroupLeader; }

  Addr getElementVAddr(uint64_t elementIdx) const;
  bool translateToPAddr(Addr vaddr, Addr &paddr) const;

  void addCredit(uint64_t n);
  void addNextRangeTailElementIdx(uint64_t rangeTailElementIdx);

  DynStreamSliceId initNextSlice();

  /**
   * Check if we have received credit for the next slice.
   */
  bool isNextSliceCredited() const {
    return this->nextAllocSliceIdx < this->creditedSliceIdx;
  }
  /**
   * Check if the next allocated slice has overflown the TotalTripCount.
   * With StreamLoopBound, we may allocated more slices beyond TotalTripCount.
   */
  bool isNextSliceOverflown() const;
  uint64_t getNextAllocSliceIdx() const { return this->nextAllocSliceIdx; }
  const DynStreamSliceId &peekNextAllocSliceId() const;
  std::pair<Addr, MachineType> peekNextAllocVAddrAndMachineType() const;
  LLCStreamSlicePtr getNextAllocSlice() const;
  LLCStreamSlicePtr allocNextSlice(LLCStreamEngine *se);

  void
  traceEvent(const ::LLVM::TDG::StreamFloatEvent::StreamFloatEventType &type);

  /**************************************************************************
   * To better managing the life cycle of LLCDynStream, instead of
   * allocating them at when StreamConfig hits the LLC SE, we allocate them
   * at once at the MLC SE. And they are released lazily at once when all
   * floating streams are terminated. Therefore, they have states:
   * INITIALIZED: Initialized by MLC SE but before LLC SE receives StreamConfig.
   * RUNNING: The stream is running at one LLC SE.
   * MIGRATING: The stream is migrating to the next LLC SE.
   * TERMINATED: The LLC SE terminated the stream.
   *
   * To correctly handle these, we have a global map from DynStreamId to
   * LLCDynStream *. We also remember the list of streams that are allocated
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
  bool isRemoteConfigured() const { return this->state != State::INITIALIZED; }

  void remoteConfigured(AbstractStreamAwareController *llcController);
  void migratingStart();
  void migratingDone(AbstractStreamAwareController *llcController);

  void terminate();

  static LLCDynStream *getLLCStream(const DynStreamId &dynId) {
    if (GlobalLLCDynStreamMap.count(dynId)) {
      return GlobalLLCDynStreamMap.at(dynId);
    } else {
      return nullptr;
    }
  }
  static LLCDynStream *getLLCStreamPanic(const DynStreamId &dynId,
                                         const char *msg = "") {
    if (auto S = LLCDynStream::getLLCStream(dynId)) {
      return S;
    }
    panic("Failed to get LLCDynStream %s: %s.", dynId, msg);
  }
  static void allocateLLCStreams(AbstractStreamAwareController *mlcController,
                                 CacheStreamConfigureVec &configs);

  bool isBasedOn(const DynStreamId &baseId) const;
  void recvStreamForward(LLCStreamEngine *se, uint64_t baseElementIdx,
                         const DynStreamSliceId &sliceId,
                         const DataBlock &dataBlk);

  bool hasComputation() const;
  StreamValue computeStreamElementValue(const LLCStreamElementPtr &element);
  void completeComputation(LLCStreamEngine *se,
                           const LLCStreamElementPtr &element,
                           const StreamValue &value);

  int getMaxInflyRequests() const { return this->maxInflyRequests; }

  std::unique_ptr<LLCStreamRangeBuilder> &getRangeBuilder() {
    return this->rangeBuilder;
  }

  /**
   * Counter to approxiate coarse-grained StreamAck.
   */
  uint64_t streamAckedSlices = 0;

private:
  State state = INITIALIZED;
  AbstractStreamAwareController *mlcController;
  AbstractStreamAwareController *llcController;

  int maxInflyRequests;

  std::unique_ptr<LLCStreamRangeBuilder> rangeBuilder;

  /**
   * Here we remember the dependent streams.
   * IndirectStreams is just the "UsedBy" dependence.
   */
  std::vector<LLCDynStreamPtr> indirectStreams;
  std::vector<LLCDynStreamPtr> allIndirectStreams;

  // Private controller as user should use allocateLLCStreams().
  LLCDynStream(AbstractStreamAwareController *_mlcController,
               AbstractStreamAwareController *_llcController,
               CacheStreamConfigureDataPtr _configData);

  static std::unordered_map<DynStreamId, LLCDynStream *, DynStreamIdHasher>
      GlobalLLCDynStreamMap;
  static std::unordered_map<NodeID, std::list<std::vector<LLCDynStream *>>>
      GlobalMLCToLLCDynStreamGroupMap;
  static LLCDynStreamPtr
  allocateLLCStream(AbstractStreamAwareController *mlcController,
                    CacheStreamConfigureDataPtr &config);

  Cycles curCycle() const;
  int curRemoteBank() const;
  const char *curRemoteMachineType() const;

  // This is really just used for memorizing in IndirectStream.
  uint64_t numElementsReadyToIssue = 0;
  uint64_t numIndirectElementsReadyToIssue = 0;
  uint64_t nextIssueElementIdx = 0;

  std::pair<Addr, MachineType> peekNextInitVAddrAndMachineType() const;
  const DynStreamSliceId &peekNextInitSliceId() const;

public:
  const CacheStreamConfigureDataPtr configData;
  SlicedDynStream slicedStream;

  // Remember the last reduction element, avoid auto releasing.
  LLCStreamElementPtr lastReductionElement = nullptr;
  // Remember the last really computed indirect reduction element.
  uint64_t lastComputedReductionElementIdx = 0;

  std::vector<CacheStreamConfigureDataPtr> sendToConfigs;

  /**
   * Remember the base stream.
   */
  void setBaseStream(LLCDynStreamPtr baseS);

  const std::vector<LLCDynStreamPtr> &getIndStreams() const {
    return this->indirectStreams;
  }
  const std::vector<LLCDynStreamPtr> &getAllIndStreams() const {
    return this->allIndirectStreams;
  }

  /**
   * Remember the basic information for BaseOn information.
   * Note: Here we are outside of CacheStreamConfigureData, so we can directly
   * store the shared_ptr without creating circular dependence.
   * Note: We may use streams from remote LLC bank, so here we just remember the
   * config.
   */
  std::vector<CacheStreamConfigureDataPtr> baseOnConfigs;

  // Base stream.
  LLCDynStream *baseStream = nullptr;

  // Root stream.
  LLCDynStream *rootStream = nullptr;

  // Dependent predicated streams.
  std::unordered_set<LLCDynStream *> predicatedStreams;

  // Base predicate stream.
  LLCDynStream *predicateStream = nullptr;

  Cycles issueClearCycle = Cycles(4);
  // Initialize cycle at MLC SE.
  const Cycles initializedCycle;
  // Last issued cycle.
  Cycles prevIssuedCycle = Cycles(0);
  // Last configure cycle at Remote SE.
  Cycles prevConfiguredCycle = Cycles(0);
  // Last migrate starting cycle.
  Cycles prevMigratedCycle = Cycles(0);

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
  LLCDynStream *multicastGroupLeader = nullptr;

  // For flow control.
  uint64_t creditedSliceIdx;
  // Next slice index to be issued.
  uint64_t nextAllocSliceIdx = 0;
  // For initialization control.
  uint64_t nextInitSliceIdx = 0;

  /**
   * Number of requests of this stream (not including indirect streams)
   * issued but data not ready.
   */
  int inflyRequests = 0;
  /**
   * Number of scheduled but incomplete computations.
   */
  int incompleteComputations = 0;

  /**
   * Map from ElementIdx to LLCStreamElement.
   */
  using IdxToElementMapT = std::map<uint64_t, LLCStreamElementPtr>;
  IdxToElementMapT idxToElementMap;

  /**
   * The elements that is predicated by this stream.
   */
  std::map<uint64_t,
           std::list<std::pair<LLCDynStream *, ConstLLCStreamElementPtr>>>
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
   * When the MLCStreamEngine sends out credits, we initialize
   * all slices immediately to simplify the implementation.
   */
  void initDirectStreamSlicesUntil(uint64_t lastSliceIdx);

  using ElementCallback = std::function<void(const DynStreamId &, uint64_t)>;

  bool isElementInitialized(uint64_t elementIdx) const;
  void registerElementInitCallback(uint64_t elementIdx,
                                   ElementCallback callback);

  bool isElementReleased(uint64_t elementIdx) const;
  uint64_t getNextUnreleasedElementIdx() const;
  LLCStreamElementPtr getElement(uint64_t elementIdx) const;
  LLCStreamElementPtr getElementPanic(uint64_t elementIdx,
                                      const char *errMsg = nullptr) const;

  /**
   * Erase the element for myself only.
   */
  void eraseElement(uint64_t elementIdx);
  void eraseElement(IdxToElementMapT::iterator elementIter);

  /**
   * Slice callback.
   */
  using SliceCallback = std::function<void(const DynStreamId &, uint64_t)>;
  void registerSliceAllocCallback(uint64_t sliceIdx, SliceCallback callback);

private:
  std::deque<LLCStreamSlicePtr> slices;

  /************************************************************************
   * State related to StreamCommit.
   * Pending StreamCommit messages.
   ************************************************************************/
  std::list<DynStreamSliceId> commitMessages;

  using ElementCallbackList = std::list<ElementCallback>;

  /**
   * Callbacks when an element is initialized.
   */
  uint64_t nextInitElementIdx = 0;
  std::map<uint64_t, ElementCallbackList> elementInitCallbacks;

  uint64_t nextCommitElementIdx = 0;
  LLCStreamCommitController *commitController = nullptr;

  /**
   * Initialize the element for myself and all UsedByStream.
   */
  void initNextElement(Addr vaddr);

  /**
   * Commit one element for myself and all the indirect streams.
   */
  void commitOneElement();

  using SliceCallbackList = std::list<SliceCallback>;
  std::map<uint64_t, SliceCallbackList> sliceAllocCallbacks;
  void invokeSliceAllocCallbacks(uint64_t sliceIdx);

public:
  void addCommitMessage(const DynStreamSliceId &sliceId);
  uint64_t getNextInitElementIdx() const { return this->nextInitElementIdx; }
  uint64_t getNextCommitElementIdx() const {
    return this->nextCommitElementIdx;
  }

  /**
   * Only indirect stream is managed in element-grainularity.
   */
  void markElementReadyToIssue(uint64_t elementIdx);
  void markElementIssued(uint64_t elementIdx);
  bool hasIndirectElementReadyToIssue() const {
    return this->numIndirectElementsReadyToIssue > 0;
  }
  size_t getNumIndirectElementReadyToIssue() const {
    return this->numIndirectElementsReadyToIssue;
  }

  /**
   * @return nullptr if no such element.
   */
  LLCStreamElementPtr getFirstReadyToIssueElement() const;

  /**
   * With range-sync, there are two issue point:
   * BeforeCommit:
   *  Streams without range-sync.
   *  Streams with range-sync, but the core need the value.
   * AfterCommit:
   *  Store/AtomicComputeStreams with range-sync.
   */
  bool shouldIssueBeforeCommit() const;
  bool shouldIssueAfterCommit() const;

  void evaluateLoopBound(LLCStreamEngine *se);
  bool hasLoopBound() const {
    return this->configData->loopBoundCallback != nullptr;
  }
  bool isLoopBoundBrokenOut() const { return this->loopBoundBrokenOut; }
  uint64_t getNextLoopBoundElemIdx() const {
    return this->nextLoopBoundElementIdx;
  }
  /**
   * Check that our LoopBound has been evaluted for this sliceId.
   * @return true if the slice is no longer needed for LoopBound.
   */
  bool isSliceDoneForLoopBound(const DynStreamSliceId &sliceId) const;

private:
  uint64_t nextLoopBoundElementIdx = 0;
  bool loopBoundBrokenOut = false;
  Addr loopBoundBrokenPAddr = 0;

public:
  /**
   * Set by LLCStreamEngine::allocateLLCStreams() and used by
   * LLCStreamMigrationController to limit stream migration and balance loads on
   * different cores.
   */
  bool isLoadBalanceValve() const { return this->loadBalanceValve; }
  void setLoadBalanceValve() { this->loadBalanceValve = true; }

private:
  bool loadBalanceValve = false;
};

#endif