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

namespace gem5 {

namespace ruby {
class AbstractStreamAwareController;
}
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
                   Addr _paddrLine, ruby::MachineType _destMachineType,
                   ruby::CoherenceRequestType _type, Cycles _issueCycle)
      : S(_S), sliceId(_sliceId), paddrLine(_paddrLine),
        destMachineType(_destMachineType), requestType(_type),
        issueCycle(_issueCycle) {}
  Stream *S;
  DynStreamSliceId sliceId;
  Addr paddrLine;
  ruby::MachineType destMachineType;
  ruby::CoherenceRequestType requestType;

  // Remember the created cycle for statistic.
  Cycles issueCycle = Cycles(0);

  bool translationDone = false;

  // Optional fields.
  ruby::DataBlock dataBlock;

  // Optional storeValueBlk. Only used for sending back to NonMigrating stream.
  ruby::DataBlock storeValueBlock;

  // Optional for StreamStore request.
  int storeSize = 8;

  // Optional for StreamForward request with smaller payload size.
  int payloadSize = ruby::RubySystem::getBlockSizeBytes();

  // Optional for Multicast request, excluding the original stream
  std::vector<DynStreamSliceId> multicastSliceIds;

  // Optional for StreamForward request, the receiver.
  DynStreamSliceId forwardToSliceId;
};

class LLCDynStream {
public:
  friend class LLCStreamRangeBuilder;
  friend class LLCStreamCommitController;

  ~LLCDynStream();

  ruby::AbstractStreamAwareController *getMLCController() const {
    return this->mlcController;
  }

  Stream *getStaticS() const { return this->configData->stream; }
  DynStream *getCoreDynS() const {
    return this->getStaticS()->getDynStream(this->getDynStreamId());
  }
  uint64_t getStaticId() const { return this->configData->dynamicId.staticId; }
  const DynStreamId &getDynStreamId() const {
    return this->getDynStrandId().dynStreamId;
  }
  const DynStrandId &getDynStrandId() const { return this->strandId; }

  int32_t getMemElementSize() const { return this->configData->elementSize; }
  int32_t getCoreElementSize() const {
    return this->getStaticS()->getCoreElementSize();
  }
  bool isPointerChase() const { return this->configData->isPointerChase; }
  bool isPseudoOffload() const { return this->configData->isPseudoOffload; }
  bool isOneIterationBehind() const {
    return this->configData->isOneIterationBehind;
  }
  bool isIndirect() const { return this->baseStream != nullptr; }
  bool isIndirectReduction() const {
    return this->getStaticS()->isIndirectReduction();
  }
  bool shouldRangeSync() const { return this->configData->rangeSync; }

  bool shouldSendValueToCore() const;

  /**
   * Predicate information.
   */
private:
  bool isPredBase = false;
  bool isPredBy = false;
  int predId = 0;
  bool predValue = false;
  DynStreamId predBaseStreamId;

public:
  bool isPredicated() const { return this->isPredBy; }
  bool getPredValue() const {
    assert(this->isPredicated());
    return this->predValue;
  }
  bool getPredId() const {
    assert(this->isPredicated());
    return this->predId;
  }
  const DynStreamId &getPredBaseStreamId() const {
    assert(this->isPredicated());
    return this->predBaseStreamId;
  }
  void evaluatePredication(LLCStreamEngine *se, uint64_t elemIdx);
  bool hasPredication() const {
    return !this->configData->predCallbacks.empty();
  }

  /**
   * Query that this stream is disabled from migration.
   */
  bool isMigrationDisabled() const {
    if (this->rootStream) {
      return this->rootStream->isMigrationDisabled();
    }
    return this->configData->disableMigration;
  }

  /**
   * We cache the TripCount from SlicedStream.
   */
  bool hasTotalTripCount() const {
    return this->totalTripCount != InvalidTripCount;
  }
  int64_t getTotalTripCount() const { return this->totalTripCount; }
  bool hasInnerTripCount() const {
    return this->innerTripCount != InvalidTripCount;
  }
  int64_t getInnerTripCount() const { return this->innerTripCount; }
  bool isInnerLastElem(uint64_t elemIdx) const;
  bool isLastElem(uint64_t elemIdx) const;
  void setTotalTripCount(int64_t totalTripCount);
  void breakOutLoop(int64_t totalTripCount);

private:
  /**
   * This always match with those TripCounts in BaseStream's SlicedDynStream.
   */
  static constexpr int64_t InvalidTripCount =
      CacheStreamConfigureData::InvalidTripCount;
  int64_t totalTripCount = InvalidTripCount;
  int64_t innerTripCount = InvalidTripCount;

public:
  /**
   * Query the offloaded machine type.
   */
  ruby::MachineType getFloatMachineTypeAtElem(uint64_t elementIdx) const;

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
  void addNextRangeTailElemIdx(uint64_t rangeTailElementIdx);

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
  bool isNextElemOverflown() const;
  uint64_t getNextAllocSliceIdx() const { return this->nextAllocSliceIdx; }
  const DynStreamSliceId &peekNextAllocSliceId() const;
  std::pair<Addr, ruby::MachineType> peekNextAllocVAddrAndMachineType() const;
  uint64_t peekNextAllocElemIdx() const { return this->nextAllocElemIdx; }
  void checkNextAllocElemIdx();
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

  void remoteConfigured(ruby::AbstractStreamAwareController *llcCtrl);
  void migratingStart(ruby::AbstractStreamAwareController *nextLLCCtrl);
  void migratingDone(ruby::AbstractStreamAwareController *llcCtrl);

  ruby::AbstractStreamAwareController *getLLCController() const {
    return this->llcController;
  }
  void setLLCController(ruby::AbstractStreamAwareController *llcController);
  ruby::AbstractStreamAwareController *curOrNextRemoteCtrl() const;

  void terminate();

  using GlobalLLCDynStreamMapT =
      std::unordered_map<DynStrandId, LLCDynStream *, DynStrandIdHasher>;
  static GlobalLLCDynStreamMapT &getGlobalLLCDynStreamMap() {
    return GlobalLLCDynStreamMap;
  }

  static LLCDynStream *getLLCStream(const DynStrandId &strandId) {
    auto iter = GlobalLLCDynStreamMap.find(strandId);
    if (iter == GlobalLLCDynStreamMap.end()) {
      return nullptr;
    }
    return iter->second;
  }
  static LLCDynStream *getLLCStreamPanic(const DynStrandId &strandId,
                                         const char *msg = "") {
    if (auto S = LLCDynStream::getLLCStream(strandId)) {
      return S;
    }
    panic("Failed to get LLCDynStream %s: %s.", strandId, msg);
  }
  static void
  allocateLLCStreams(ruby::AbstractStreamAwareController *mlcController,
                     CacheStreamConfigureVec &configs);

  bool isBasedOn(const DynStreamId &baseId) const;
  void recvStreamForward(LLCStreamEngine *se, int offset,
                         const DynStreamSliceId &sliceId,
                         const DynStreamSliceId &sendToSliceId,
                         const ruby::DataBlock &dataBlk);

  bool hasComputation() const;
  StreamValue computeElemValue(const LLCStreamElementPtr &element);
  void completeComputation(LLCStreamEngine *se, const LLCStreamElementPtr &elem,
                           const StreamValue &value);
  void tryComputeNextDirectReduceElem(LLCStreamEngine *se,
                                      const LLCStreamElementPtr &elem);
  void tryComputeNextIndirectReduceElem(LLCStreamEngine *se);
  void completeFinalReduce(LLCStreamEngine *se, uint64_t elemIdx);
  void completeIndReduceElem(LLCStreamEngine *se,
                             const LLCStreamElementPtr &elem);

  int getMaxInflyRequests() const { return this->maxInflyRequests; }

  std::unique_ptr<LLCStreamRangeBuilder> &getRangeBuilder() {
    return this->rangeBuilder;
  }

  /**
   * Counter to approxiate coarse-grained StreamAck.
   * Normally without RangeSync, we would send out one Ack per
   * slice. However, we could just sent this out coarse grained.
   * For simplicity, here I just hack by force some idea ack.
   */
  bool isNextIdeaAck() const;
  void ackedOneSlice() { streamAckedSlices++; }

  void doneOnePUMPrefetchSlice() { this->pumPrefetchDoneSlices++; }
  uint64_t getPUMPrefetchDoneSlices() const {
    return this->pumPrefetchDoneSlices;
  }

private:
  uint64_t streamAckedSlices = 0;
  uint64_t pumPrefetchDoneSlices = 0;

  State state = INITIALIZED;
  ruby::AbstractStreamAwareController *mlcController;
  ruby::AbstractStreamAwareController *llcController;
  ruby::AbstractStreamAwareController *nextLLCController;

  int maxInflyRequests;

  std::unique_ptr<LLCStreamRangeBuilder> rangeBuilder;

  /**
   * Here we remember the dependent streams.
   * IndirectStreams is just the "UsedBy" dependence.
   * Also record the reuse of BaseElem.
   */
  std::vector<LLCDynStreamPtr> indirectStreams;
  std::vector<LLCDynStreamPtr> allIndirectStreams;

  // Private controller as user should use allocateLLCStreams().
  LLCDynStream(ruby::AbstractStreamAwareController *_mlcController,
               ruby::AbstractStreamAwareController *_llcController,
               CacheStreamConfigureDataPtr _configData);

  static GlobalLLCDynStreamMapT GlobalLLCDynStreamMap;
  static std::unordered_map<ruby::NodeID,
                            std::list<std::vector<LLCDynStream *>>>
      GlobalMLCToLLCDynStreamGroupMap;
  static LLCDynStreamPtr
  allocateLLCStream(ruby::AbstractStreamAwareController *mlcController,
                    CacheStreamConfigureDataPtr &config);

  Cycles curCycle() const;
  int curRemoteBank() const;
  const char *curRemoteMachineType() const;

  // This is really just used for memorizing in IndirectStream.
  uint64_t numElemsReadyToIssue = 0;
  uint64_t numDepIndElemsReadyToIssue = 0;
  uint64_t nextAllocElemIdx = 0;
  uint64_t nextIssueElemIdx = 0;

public:
  uint64_t getNextIssueElemIdx() const { return this->nextIssueElemIdx; }
  void skipIssuingPredOffElems();

private:
  std::pair<Addr, ruby::MachineType> peekNextInitVAddrAndMachineType() const;
  const DynStreamSliceId &peekNextInitSliceId() const;
  uint64_t peekNextInitElemIdx() const;

public:
  const CacheStreamConfigureDataPtr configData;
  SlicedDynStream slicedStream;
  DynStrandId strandId;

  // Remember the last reduction element, avoid auto releasing.
  LLCStreamElementPtr lastReductionElement = nullptr;
  // Remember the last really computed indirect reduction element.
  uint64_t lastReducedElemIdx = 0;

  std::vector<CacheStreamConfigureData::DepEdge> sendToEdges;
  std::vector<CacheStreamConfigureData::DepEdge> sendToPUMEdges;
  // Number of PUMData packets sent to each bank.
  int64_t sentPUMPackets = 0;
  std::map<ruby::NodeID, int> sentPUMDataPacketMap;

  /**
   * Remember the base stream.
   */
  void setBaseStream(LLCDynStreamPtr baseS, int reuse);

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
   * NOTE: This is one-to-one mapping between baseOnConfigs <-> baseEdges.
   */
  std::vector<CacheStreamConfigureDataPtr> baseOnConfigs;

  /**
   * Remember the currently reused BaseElement.
   */
  struct ReusedBaseElement {
    const int reuse = 1;
    uint64_t streamElemIdx = 0;
    LLCStreamElementPtr elem = nullptr;
    ReusedBaseElement(int _reuse) : reuse(_reuse) {}
  };
  std::vector<ReusedBaseElement> reusedBaseElems;

  // Base stream with reuse.
  LLCDynStream *baseStream = nullptr;
  int baseStreamReuse = 1;

  // Root stream.
  LLCDynStream *rootStream = nullptr;

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

  // Exclude uncredited slices.
  uint64_t getNumUncreditedSlices() const {
    // Always initialized before credited.
    assert(this->nextInitSliceIdx >= this->creditedSliceIdx);
    return this->nextInitSliceIdx - this->creditedSliceIdx;
  }

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

  using ElementCallback = std::function<void(const DynStrandId &, uint64_t)>;

  bool isElemInitialized(uint64_t elementIdx) const;
  void registerElemInitCallback(uint64_t elementIdx, ElementCallback callback);

  bool isElemReleased(uint64_t elementIdx) const;
  void registerElemPostReleaseCallback(uint64_t elementIdx,
                                       ElementCallback callback);
  uint64_t getNextUnreleasedElemIdx() const;
  LLCStreamElementPtr getElem(uint64_t elementIdx) const;
  LLCStreamElementPtr getElemPanic(uint64_t elementIdx,
                                   const char *errMsg = nullptr) const;

  /**
   * Erase the element for myself only.
   */
  void eraseElem(uint64_t elemIdx);
  void eraseElem(IdxToElementMapT::iterator elemIter);
  void invokeElemPostReleaseCallback(uint64_t elemIdx);

  /**
   * Slice callback.
   */
  using SliceCallback = std::function<void(const DynStreamId &, uint64_t)>;
  void registerSliceAllocCallback(uint64_t sliceIdx, SliceCallback callback);

private:
  std::deque<LLCStreamSlicePtr> slices;
  DynStreamSliceId lastAllocSliceId;

  /************************************************************************
   * State related to StreamCommit.
   * Pending StreamCommit messages.
   ************************************************************************/
  std::list<DynStreamSliceId> commitMessages;

  using ElementCallbackList = std::list<ElementCallback>;

  /**
   * Callbacks when an element is initialized.
   */
  uint64_t nextInitStrandElemIdx = 0;
  std::map<uint64_t, ElementCallbackList> elemInitCallbacks;

  /**
   * Callbacks when an element is released.
   */
  std::map<uint64_t, ElementCallbackList> elemPostReleaseCallbacks;

  uint64_t nextCommitElemIdx = 0;
  LLCStreamCommitController *commitController = nullptr;

  /**
   * Initialize the element for myself and all UsedByStream.
   */
  void initNextElem(Addr vaddr);

  /**
   * Commit one element for myself and all the indirect streams.
   */
  void commitOneElement();

  using SliceCallbackList = std::list<SliceCallback>;
  std::map<uint64_t, SliceCallbackList> sliceAllocCallbacks;
  void invokeSliceAllocCallbacks(uint64_t sliceIdx);

public:
  void addCommitMessage(const DynStreamSliceId &sliceId);
  uint64_t getNextInitElementIdx() const { return this->nextInitStrandElemIdx; }
  uint64_t getNextCommitElementIdx() const { return this->nextCommitElemIdx; }

  /**
   * Only indirect stream is managed in element-grainularity.
   */
  void markElemReadyToIssue(uint64_t elemIdx);
  void markElemIssued(uint64_t elemIdx);
  bool hasElemReadyToIssue() const { return this->numElemsReadyToIssue > 0; }
  size_t getNumElemReadyToIssue() const { return this->numElemsReadyToIssue; }
  bool hasDepIndElemReadyToIssue() const {
    return this->numDepIndElemsReadyToIssue > 0;
  }
  size_t getNumDepIndElemReadyToIssue() const {
    return this->numDepIndElemsReadyToIssue;
  }

  /**
   * @return nullptr if no such element.
   */
  LLCStreamElementPtr getFirstReadyToIssueElem() const;

  /**
   * With range-sync, there are two issue point:
   * BeforeCommit:
   *  Streams without range-sync.
   *  Streams with range-sync, but the core need the value.
   * AfterCommit:
   *  Store/AtomicComputeStreams with range-sync.
   */
  bool issueBeforeCommit = true;
  bool issueAfterCommit = false;
  bool shouldIssueBeforeCommit() const { return this->issueBeforeCommit; }
  bool shouldIssueAfterCommit() const { return this->issueAfterCommit; }
  bool checkIssueBeforeCommit() const;
  bool checkIssueAfterCommit() const;

  void evaluateLoopBound(LLCStreamEngine *se, uint64_t elemIdx);
  bool hasLoopBound() const {
    return this->configData->loopBoundCallback != nullptr;
  }
  bool isLoopBoundBrokenOut() const { return this->loopBoundBrokenOut; }
  /**
   * Check that our LoopBound has been evaluted for this sliceId.
   * @return true if the slice is no longer needed for LoopBound.
   */
  bool isSliceDoneForLoopBound(const DynStreamSliceId &sliceId) const;

private:
  bool loopBoundBrokenOut = false;

public:
  /**
   * Set by LLCStreamEngine::allocateLLCStreams() and used by
   * LLCStreamMigrationController to limit stream migration and balance loads on
   * different cores.
   */
  bool isLoadBalanceValve() const { return this->loadBalanceValve; }
  void setLoadBalanceValve() { this->loadBalanceValve = true; }

  void sample();

private:
  bool loadBalanceValve = false;
};

} // namespace gem5

#endif