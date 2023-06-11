#ifndef __GEM_FORGE_ACCELERATOR_STREAM_HH__
#define __GEM_FORGE_ACCELERATOR_STREAM_HH__

#include "cache/CacheStreamConfigureData.hh"
#include "cpu/gem_forge/gem_forge_utils.hh"
#include "cpu/gem_forge/llvm_insts.hh"
#include "dyn_stream.hh"
#include "stream_atomic_op.hh"
#include "stream_element.hh"
#include "stream_float_tracer.hh"
#include "stream_history.hh"
#include "stream_pattern.hh"
#include "stream_statistic.hh"

#include "base/types.hh"
#include "mem/packet.hh"

#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif

#include "StreamMessage.pb.h"

#include <list>
#include <vector>

namespace gem5 {

class LLVMTraceCPU;
class GemForgeCPUDelegator;

class StreamEngine;
class StreamConfigInst;
class StreamEndInst;

/**
 * A LogicalStream is a simple wrap around the StreamInfo,
 * and is the basic unit of coalescing.
 */
class LogicalStream {
public:
  using StaticId = DynStreamId::StaticId;

  LogicalStream(const std::string &_traceExtraFolder,
                const LLVM::TDG::StreamInfo &_info);

  LogicalStream(const LogicalStream &Other) = delete;
  LogicalStream(LogicalStream &&Other) = delete;
  LogicalStream &operator=(const LogicalStream &Other) = delete;
  LogicalStream &operator=(LogicalStream &&Other) = delete;

  ~LogicalStream() {}

  using PredicatedStreamIdList =
      ::google::protobuf::RepeatedPtrField<::LLVM::TDG::PredicatedStreamId>;
  using StreamIdList =
      ::google::protobuf::RepeatedPtrField<::LLVM::TDG::StreamId>;
  using StreamInfoType = ::LLVM::TDG::StreamInfo_Type;
  using ExecFuncInfo = ::LLVM::TDG::ExecFuncInfo;

  StaticId getStreamId() const { return this->info.id(); }
  StreamInfoType getStreamType() const { return this->info.type(); }
  uint32_t getLoopLevel() const {
    return this->info.static_info().loop_level();
  }
  uint32_t getConfigLoopLevel() const {
    return this->info.static_info().config_loop_level();
  }
  bool getIsInnerMostLoop() const {
    return this->info.static_info().is_inner_most_loop();
  }
  bool getIsConditional() const {
    return this->info.static_info().is_cond_access();
  }
  bool getIsNonSpec() const {
    for (const auto &p : this->info.static_info().user_param()) {
      if (p == "non-spec") {
        return true;
      }
    }
    return false;
  }
  bool getFloatManual() const {
    return this->info.static_info().float_manual() ==
           ::LLVM::TDG::StaticStreamInfo_ManualFloatType_FLOAT;
  }
  bool getNotFloatManual() const {
    return this->info.static_info().float_manual() ==
           ::LLVM::TDG::StaticStreamInfo_ManualFloatType_NOT_FLOAT;
  }
  uint64_t getCoalesceBaseStreamId() const {
    return this->info.coalesce_info().base_stream();
  }
  int32_t getCoalesceOffset() const {
    return this->info.coalesce_info().offset();
  }
  int32_t getMemElementSize() const {
    return this->info.static_info().mem_element_size();
  }
  int32_t getCoreElementSize() const {
    return this->info.static_info().core_element_size();
  }
  const PredicatedStreamIdList &getPredicatedStreams() const {
    return this->info.static_info().predicated_streams();
  }
  const PredicatedStreamIdList &getMergedPredicatedStreams() const {
    return this->info.static_info().merged_predicated_streams();
  }
  bool isMergedPredicated() const {
    return this->info.static_info().is_merged_predicated_stream();
  }
  bool isMergedLoadStoreDepStream() const {
    /**
     * TODO: Get rid of this unclean "merged" implementation.
     */
    return false;
    // return this->info.static_info().compute_info().value_base_streams_size()
    // >
    //        0;
  }
  const StreamIdList &getMergedLoadStoreDepStreams() const {
    return this->info.static_info().compute_info().value_dep_streams();
  }
  const StreamIdList &getMergedLoadStoreBaseStreams() const {
    return this->info.static_info().compute_info().value_base_streams();
  }
  const ExecFuncInfo &getStoreFuncInfo() const {
    return this->info.static_info().compute_info().store_func_info();
  }
  const ExecFuncInfo &getLoadFuncInfo() const {
    return this->info.static_info().compute_info().load_func_info();
  }
  bool getEnabledStoreFunc() const {
    return this->info.static_info().compute_info().enabled_store_func();
  }
  bool getEnabledLoadFunc() const {
    return this->getLoadFuncInfo().name() != "";
  }
  LLVM::TDG::ExecFuncInfo_ComputeOp getAddrFuncComputeOp() const {
    return this->info.addr_func_info().compute_op();
  }
  bool getReduceFromZero() const {
    return this->info.static_info().compute_info().reduce_from_zero();
  }
  bool isLoopEliminated() const {
    return this->info.static_info().loop_eliminated();
  }
  bool isInnerFinalValueUsedByCore() const {
    return this->info.static_info().core_need_final_value();
  }
  bool isInnerSecondFinalValueUsedByCore() const {
    return this->info.static_info().core_need_second_final_value();
  }
  bool isTripCountFixed() const {
    return this->info.static_info().is_trip_count_fixed();
  }

  LLVM::TDG::StreamInfo info;
  std::unique_ptr<StreamHistory> history;
  std::unique_ptr<StreamPattern> patternStream;
};

/**
 * Holdes the aggregated stream state, across multiple dynamic streams.
 * This may also contain multiple coalesced static LogicalStreams.
 */
class Stream {
public:
  friend class DynStream;
  using StaticId = DynStreamId::StaticId;
  using InstanceId = DynStreamId::InstanceId;
  struct StreamArguments {
    LLVMTraceCPU *cpu;
    GemForgeCPUDelegator *cpuDelegator;
    StreamEngine *se;
    int maxSize;
    const ::LLVM::TDG::StreamRegion *streamRegion;
    StaticId staticId;
    const char *name;
  };

  Stream(const StreamArguments &args);
  virtual ~Stream();

  /**
   * Stream initialization is divided into 3 phases:
   * 1. Create the basic unit -- as the place holder.
   * 2. Finalize it.
   *    a. For coalesced stream -- choose the prime logical stream.
   *    b. Find base streams.
   *    c. Find back base streams.
   *    d. Find AliasBaseStream and AliasedStreams.
   * Notice that some information are not valid until finalized, e.g.
   * StreamName, StaticId.
   * 3. Fix dependence on inner-loop stream.
   *    So far this only works when the inner-loop is nested.
   */
  void addStreamInfo(const LLVM::TDG::StreamInfo &info);
  void finalize();
  void postFinalize();
  void fixInnerLoopBaseStreams();
  void addBaseStepStream(Stream *baseStepStream);
  void registerStepDependentStreamToRoot(Stream *newDependentStream);
  void
  initializeAliasStreamsFromProtobuf(const ::LLVM::TDG::StaticStreamInfo &info);
  void initializeCoalesceGroupStreams();

  const std::string &getStreamName() const { return this->streamName; }
  bool isAtomicStream() const;
  bool isStoreStream() const;
  bool isLoadStream() const;
  bool isUpdateStream() const;
  bool isMemStream() const;
  bool isAffineIVStream() const;
  bool isDirectLoadStream() const;
  bool isDirectStoreStream() const;
  bool isIndirectLoadStream() const;
  bool isDirectMemStream() const;
  bool shouldComputeValue() const;
  bool isAtomicComputeStream() const {
    return this->isAtomicStream() && this->getEnabledStoreFunc();
  }
  bool isStoreComputeStream() const {
    return this->isStoreStream() && this->getEnabledStoreFunc();
  }
  bool isLoadComputeStream() const {
    return this->isLoadStream() && this->getEnabledLoadFunc();
  }
  bool trackedByPEB() const {
    return this->isLoadStream() && !this->getFloatManual();
  }

  /**
   * Check if the stream is configured. Due to nest streams,
   * now StreamConfig and StreamEnd are not properly interleaved.
   * But StreamConfig and StreamEnd are still in-order within its
   * own class. Thus we define that a stream is configured if it
   * has some DynStreams, and the last one has not dispatched
   * the StreamEnd.
   */
  bool isConfigured() const;
  /**
   * Head is the newest element.
   * Tail is the dummy node before the oldest element.
   */
  size_t stepSize;
  size_t maxSize;
  void incrementMaxSize(size_t delta) {
    this->maxSize += delta;
    this->freeElemCallbacks.invokeCallback(this);
  }

  int lateFetchCount;
  int numInflyStreamRequests = 0;
  void incrementInflyStreamRequest() { this->numInflyStreamRequests++; }
  void decrementInflyStreamRequest() {
    assert(this->numInflyStreamRequests > 0);
    this->numInflyStreamRequests--;
  }

  const ::LLVM::TDG::StreamRegion *streamRegion;
  StaticId staticId;
  std::string streamName;
  InstanceId dynInstance;
  StreamStatistic statistic;
  // Used to remember first core user pc.
  Addr firstCoreUserPC = 0;
  bool hasFirstCoreUserPC() const { return this->firstCoreUserPC != 0; }
  Addr getFirstCoreUserPC() const { return this->firstCoreUserPC; }
  void setFirstCoreUserPC(Addr firstCoreUserPC) {
    this->firstCoreUserPC = firstCoreUserPC;
  }

  /**
   * Step root stream, three possible cases:
   * 1. this: I am the step root.
   * 2. other: I am controlled by other step stream.
   * 3. nullptr: I am a constant stream.
   */
  Stream *stepRootStream;
  using StreamSet = std::unordered_set<Stream *>;
  using StreamVec = std::vector<Stream *>;

  /**
   * Represent stream dependence. Due to coalescing, there maybe multiple
   * edges between two streams. e.g. b[i] = a[i] + a[i - 1].
   */
  using PredFuncId = int;
  struct StreamDepEdge {
    using TypeE = DynStreamDepEdge::TypeE;
    const TypeE type;
    const StaticId fromStaticId = DynStreamId::InvalidStaticStreamId;
    const StaticId toStaticId = DynStreamId::InvalidStaticStreamId;
    Stream *const toStream = nullptr;
    // Only used for PredEdge.
    const PredFuncId predFuncId = 0;
    const bool predValue = false;
    StreamDepEdge(TypeE _type, StaticId _fromId, StaticId _toId,
                  Stream *_toStream, PredFuncId _predFuncId = 0,
                  bool _predValue = false)
        : type(_type), fromStaticId(_fromId), toStaticId(_toId),
          toStream(_toStream), predFuncId(_predFuncId), predValue(_predValue) {}
  };
  using StreamEdges = std::vector<StreamDepEdge>;
  void addBaseStream(StreamDepEdge::TypeE type, bool isInnerLoop,
                     StaticId baseId, StaticId depId, Stream *baseS);
  void addPredBaseStream(PredFuncId predId, bool predValue, bool isInnerLoop,
                         StaticId baseId, StaticId depId, Stream *baseS);
  PredFuncId getPredFuncId(StaticId depId);
  bool getPredValue(StaticId depId);

  StreamEdges baseEdges;
  StreamEdges depEdges;

  StreamSet addrBaseStreams;
  StreamSet addrDepStreams;

  StreamSet valueBaseStreams;
  StreamSet valueDepStreams;

  StreamSet predBaseStreams;
  StreamSet predDepStreams;

  /**
   * Back edge dependence on previous iteration.
   */
  StreamSet backBaseStreams;
  StreamSet backDepStreams;
  bool hasBackDepReductionStream = false;
  bool isBoundedByPointerChase = false;

  /**
   * Dependence on inner-loop streams.
   */
  StreamEdges innerLoopBaseEdges;
  StreamEdges innerLoopDepEdges;

  /**
   * Whether we have non-core dependents.
   * This is used to determine if we can do selective flush.
   */
  bool hasNonCoreDependent() const {
    if (this->addrDepStreams.empty() && this->valueDepStreams.empty() &&
        this->backDepStreams.empty()) {
      return false;
    }
    return true;
  }
  /**
   * Alias stream information.
   * AliasBaseStream can be:
   * 1. nullptr -> not memory stream.
   * 2. this    -> I am the leader of the alias group.
   * 3. other   -> I am a follower in the alias group.
   */
  Stream *aliasBaseStream = nullptr;
  int32_t aliasOffset = 0;
  StreamVec aliasedStreams;
  bool hasAliasedStoreStream = false;
  /**
   * Coalesce stream information, including this.
   */
  StreamSet coalesceGroupStreams;

  /**
   * Per stream statistics.
   */
  StreamFloatTracer floatTracer;
  void dumpStreamStats(std::ostream &os) const;

  LLVMTraceCPU *getCPU() { return this->cpu; }
  GemForgeCPUDelegator *getCPUDelegator() const;
  StreamEngine *getSE() const;
  int getCPUId() { return this->getCPUDelegator()->cpuId(); }

  void configure(uint64_t seqNum, ThreadContext *tc);

  void dispatchStreamConfig(uint64_t seqNum, ThreadContext *tc);
  void executeStreamConfig(uint64_t seqNum, const DynStreamParamV *inputVec);
  void commitStreamConfig(uint64_t seqNum);
  void rewindStreamConfig(uint64_t seqNum);
  bool isStreamConfigureExecuted(uint64_t seqNum);

  void releaseDynStream(uint64_t configSeqNum);

  /***********************************************************************
   * API to manage the elements of this stream.
   ***********************************************************************/
  /**
   * Get the total number of allocated elements among all dynamic streams.
   */
  int getAllocSize() const { return this->allocSize; }

  void decrementAllocSize() {
    this->allocSize--;
    this->freeElemCallbacks.invokeCallback(this);
  }

  /**
   * Callback when there is free element.
   */
  using StreamCallback = std::function<bool(Stream *)>;
  CallbackList<StreamCallback> freeElemCallbacks;
  void registerFreeElemCallback(StreamCallback callback) {
    freeElemCallbacks.registerCallback(callback);
  }

  /**
   * Allocate a new dynamic instance with FIFOIdx.
   */
  DynStreamId allocateNewInstance();

  /**
   * Check if the dynamic stream has an unstepped element.
   * @param instanceId: if Invalid, check FirstAliveDynStream.
   */
  bool hasUnsteppedElement(DynStreamId::InstanceId instanceId);
  /**
   * Get the first unstepped element of the last dynamic stream.
   */
  StreamElement *getFirstUnsteppedElement();
  /**
   * Get the first alive dynamic stream (End not dispatched).
   */
  DynStream &getFirstAliveDynStream();
  /**
   * Try to get the first alive dynamic stream (End not dispatched).
   */
  DynStream *tryGetFirstAliveDynStream();
  /**
   * Get the current allocating dynamic stream (may be nullptr).
   * 1. End not dispatched.
   * 2. If has TotalTripCount, it has not reached that limit.
   */
  DynStream *getAllocatingDynStream();
  /**
   * Get previous element in the chain of the stream.
   * Notice that it may return nullptr if this is
   * the first element for that stream.
   */
  StreamElement *getPrevElement(StreamElement *element);
  void handleMergedPredicate(const DynStream &dynS, StreamElement *element);
  void performStore(const DynStream &dynS, StreamElement *element,
                    uint64_t storeValue);

  /**
   * Called by executeStreamConfig() to allow derived class to set up the
   * AddrGenCallback in DynStream.
   */
  void setupAddrGen(DynStream &dynStream, const DynStreamParamV *inputVec);

  /**
   * Extract extra input values from the inputVec. May modify inputVec.
   */
  void extractExtraInputValues(DynStream &dynS, DynStreamParamV *inputVec);

  /**
   * ! Hack: LLVM may generate crazy trip count expression that does not
   * ! consider the case when TripCount could be negative or zero, as it
   * ! assumes such case is covered by some branch checking.
   * ! As a hack here, we check the most significant 8 bits, and if they
   * ! are set, we set the TripCount to 0.
   */
  int64_t fixSuspiciousTripCount(int64_t tripCount) const;

  /**
   * For debug.
   */
  std::string dumpString() const;
  void dump() const;
  void sampleStatistic();

  /**
   * Allocate the CacheStreamConfigureData.
   */
  CacheStreamConfigureDataPtr
  allocateCacheConfigureData(uint64_t configSeqNum, bool isIndirect = false);

  /**
   * Allocate the CacheStreamConfigureData for UsedAffineIV.
   */
  CacheStreamConfigureDataPtr
  allocateCacheConfigureDataForAffineIV(uint64_t configSeqNum);

  /**
   * We may take reference of DynS in callbacks. Use list.
   */
  std::list<DynStream> dynamicStreams;
  std::unordered_map<InstSeqNum, DynStream *> configSeqNumToDynStreamMap;
  bool hasDynStream() const { return !this->dynamicStreams.empty(); }
  size_t getNumDynStreams() const { return this->dynamicStreams.size(); }
  DynStream &getDynStream(InstSeqNum seqNum) {
    auto iter = this->configSeqNumToDynStreamMap.find(seqNum);
    assert(iter != this->configSeqNumToDynStreamMap.end() &&
           "Failed to find DynStream by ConfigSeqNum.");
    return *iter->second;
  }
  DynStream &getDynStreamByEndSeqNum(InstSeqNum seqNum);
  DynStream &getDynStreamByInstance(InstanceId instance);
  DynStream *tryGetDynStreamByInstance(InstanceId instance);
  DynStream &getDynStreamBefore(InstSeqNum seqNum);
  DynStream *getDynStream(const DynStreamId &dynId);
  DynStream &getLastDynStream() {
    assert(!this->dynamicStreams.empty() && "No dynamic stream.");
    return this->dynamicStreams.back();
  }
  DynStream &getFirstDynStream() {
    assert(!this->dynamicStreams.empty() && "No dynamic stream.");
    return this->dynamicStreams.front();
  }
  /**
   * Record the last a few rewinded DynStream instances for debug purpose.
   */
  std::deque<DynStreamId::InstanceId> rewindedInstances;
  void recordRewindedDynS(const DynStreamId &dynStreamId);
  bool isDynStreamRewinded(const DynStreamId &dynStreamId) const;

  LLVMTraceCPU *cpu;
  StreamEngine *se;

  /**
   * StreamAggregateHistory. This is used to detect reuse
   * across stream configuration.
   */
  struct StreamAggregateHistory {
    DynStreamFormalParamV addrGenFormalParams;
    uint64_t numReleasedElements = 0;
    uint64_t numIssuedRequests = 0;
    uint64_t numPrivateCacheHits = 0;
    uint64_t startVAddr = 0;
    bool floated = false;
  };
  static constexpr int AggregateHistorySize = 4;
  std::list<StreamAggregateHistory> aggregateHistory;
  void recordAggregateHistory(const DynStream &dynS);

  AddrGenCallbackPtr &getAddrGenCallback() { return this->addrGenCallback; }

  const ExecFuncPtr &getComputeCallback() const;

  bool isComputationNop() const { return this->getComputeCallback()->isNop(); }
  int getComputationNumMicroOps() const;
  Cycles getEstimatedComputationLatency() const;
  bool isSIMDComputation() const {
    return this->getComputeCallback()->hasSIMD();
  }
  enum ComputationType {
    UnknownComputationType = 0,
    LoadCompute,
    StoreCompute,
    AtomicCompute,
    Update,
    Reduce,
    Address,
  };
  enum ComputationAddressPattern {
    Affine,
    Indirect,
    PointerChase,
    MultiAffine,
  };
  using ComputationCategory =
      std::pair<ComputationType, ComputationAddressPattern>;
  ComputationCategory getComputationCategory() const;
  mutable ComputationCategory memorizedComputationCategory;
  mutable bool computationCategoryMemorized = false;

  /**
   * Add the computation to core statistic.
   */
  void recordComputationInCoreStats() const;

  std::unique_ptr<StreamAtomicOp>
  setupAtomicOp(FIFOEntryIdx idx, int memElementsize,
                const DynStreamFormalParamV &formalParams,
                GetStreamValueFunc getStreamValue);

  bool hasDepNestRegion() const { return this->depNestRegion; }
  void setDepNestRegion() { this->depNestRegion = true; }

  /**
   * This is used to record offloaded stream's progress.
   */
  void incrementOffloadedStepped();

  /**
   * Manage SkipAllocDynS Count.
   */
  int getNumSkipAllocDynS() const { return this->numSkipAllocDynS; }
  bool skipAlloc() const {
    return this->getNumSkipAllocDynS() == this->getNumDynStreams();
  }
  void incrementSkipAllocDynS() { this->numSkipAllocDynS++; }
  void decrementSkipAllocDynS() {
    assert(this->numSkipAllocDynS > 0);
    this->numSkipAllocDynS--;
  }

protected:
  StreamSet baseStepStreams;
  StreamSet baseStepRootStreams;
  StreamSet depStepStreams;

  AddrGenCallbackPtr addrGenCallback;
  ExecFuncVec predCallbacks;
  ExecFuncPtr storeCallback;
  ExecFuncPtr loadCallback;

  /**
   * Remember if this is a nest stream.
   */
  bool nested = false;

  /**
   * Remember if this stream has dependent nest stream region.
   */
  bool depNestRegion = false;

  /**
   * Total allocated elements among all dynamic streams.
   */
  size_t allocSize;

  /**
   * Number of skip alloc dynS.
   */
  int numSkipAllocDynS = 0;

  /**
   * Step the dependent streams in this order.
   */
  std::list<Stream *> stepStreamList;

  bool isStepRoot() const {
    auto type = this->getStreamType();
    return this->baseStepStreams.empty() &&
           (type == ::LLVM::TDG::StreamInfo_Type_IV ||
            type == ::LLVM::TDG::StreamInfo_Type_ST);
  }

  /**
   * Helper function to setup a linear addr func.
   */
  void setupLinearAddrFunc(DynStream &dynStream,
                           const DynStreamParamV *inputVec,
                           const LLVM::TDG::StreamInfo &info);
  /**
   * Helper function to setup an func addr gen.
   */
  void setupFuncAddrFunc(DynStream &dynStream, const DynStreamParamV *inputVec,
                         const LLVM::TDG::StreamInfo &info);
  /**
   * Helper function to setup formal params for an ExecFunc.
   * @return Number of input value consumed.
   */
  int setupFormalParams(const DynStreamParamV *inputVec,
                        const LLVM::TDG::ExecFuncInfo &info,
                        DynStreamFormalParamV &formalParams);

  /***************************************************************
   * Managing coalesced LogicalStream within this one.
   * The first one is "prime stream", whose stream id is used to represent
   * this stream.
   *
   * Most properties are directly stored in PrimeLogical, except:
   * 1. NoCoreUsers: True iff. all have no core users.
   ***************************************************************/
  std::vector<LogicalStream *> logicals;
  LogicalStream *primeLogical = nullptr;
  int32_t coalescedMemElemSize = -1;
  int32_t coalescedCoreElemSize = -1;
  int32_t baseOffset = -1;
  bool coalescedNoCoreUser = true;
  bool isNonSpec = false;
  bool isIndReduce = false;

  void selectPrimeLogicalStream();
  void initializeBaseStreams();
  void initializeAliasStreams();

public:
  /********************************************************************
   * Static information accessor.
   ********************************************************************/
  using PredicatedStreamIdList = LogicalStream::PredicatedStreamIdList;
  using StreamInfoType = LogicalStream::StreamInfoType;
  using StreamIdList = LogicalStream::StreamIdList;
  using ExecFuncInfo = LogicalStream::ExecFuncInfo;
#define Get(T, Name)                                                           \
  T get##Name() const { return this->primeLogical->get##Name(); }
#define Is(Name)                                                               \
  bool is##Name() const { return this->primeLogical->is##Name(); }

  Get(StreamInfoType, StreamType);
  Get(uint32_t, LoopLevel);
  Get(uint32_t, ConfigLoopLevel);
  Get(bool, IsInnerMostLoop);
  Get(bool, IsConditional);
  Get(bool, FloatManual);
  Get(bool, NotFloatManual);
  Get(const PredicatedStreamIdList &, MergedPredicatedStreams);
  Get(const StreamIdList &, MergedLoadStoreDepStreams);
  Get(const StreamIdList &, MergedLoadStoreBaseStreams);
  Get(const ExecFuncInfo &, StoreFuncInfo);
  Get(const ExecFuncInfo &, LoadFuncInfo);
  Get(bool, EnabledStoreFunc);
  Get(bool, EnabledLoadFunc);
  Get(bool, ReduceFromZero);
  Get(::LLVM::TDG::ExecFuncInfo_ComputeOp, AddrFuncComputeOp);
  Is(MergedPredicated);
  Is(MergedLoadStoreDepStream);
  Is(LoopEliminated);
  Is(InnerFinalValueUsedByCore);
  Is(InnerSecondFinalValueUsedByCore);
  Is(TripCountFixed);

  /**
   * Get the coalesce base stream and offset.
   * NOTE: This stream may not be coalesced into the base due to
   * NOTE: large offset (see stream_engine.cc).
   */
  Get(uint64_t, CoalesceBaseStreamId);
  Get(int32_t, CoalesceOffset);

#undef Get
#undef Is

  bool isReductionDistributable() const;

  int32_t getMemElementSize() const {
    assert(this->coalescedMemElemSize > 0 && "Invalid MemElem size.");
    return this->coalescedMemElemSize;
  }
  int32_t getCoreElementSize() const {
    assert(this->coalescedCoreElemSize > 0 && "Invalid CoreElem size.");
    return this->coalescedCoreElemSize;
  }

  bool isMerged() const {
    return this->isMergedPredicated() || this->isMergedLoadStoreDepStream();
  }

  std::vector<StaticId> getLogicalStreamIds() const {
    std::vector<StaticId> ret;
    for (const auto &logicS : this->logicals) {
      ret.push_back(logicS->getStreamId());
    }
    return ret;
  }
  size_t getNumLogicalStreams() const { return this->logicals.size(); }
  bool isSingle() const { return this->getNumLogicalStreams() == 1; }

  bool hasUpdate() const {
    return this->primeLogical->info.static_info()
               .compute_info()
               .update_stream()
               .id() != DynStreamId::InvalidStaticStreamId;
  }

  const ::LLVM::TDG::StreamParam &getConstUpdateParam() const {
    assert(
        this->isSingle() &&
        "Do not support constant update for more than 1 coalesced stream yet.");
    return this->primeLogical->info.static_info().const_update_param();
  }

  bool isReduction() const {
    if (this->primeLogical->info.static_info().val_pattern() ==
        ::LLVM::TDG::StreamValuePattern::REDUCTION) {
      assert(this->isSingle() &&
             "CoalescedStream should never be reduction stream.");
      return true;
    }
    return false;
  }
  bool isIndirectReduction() const { return this->isIndReduce; }

  bool isPointerChase() const {
    return this->primeLogical->info.static_info().val_pattern() ==
           ::LLVM::TDG::StreamValuePattern::POINTER_CHASE;
  }

  bool isPointerChaseIndVar() const {
    if (this->isPointerChase() && !this->isMemStream()) {
      assert(this->isSingle() &&
             "CoalescedStream should never be PointerChaseIndVarStream.");
      return true;
    }
    return false;
  }

  bool isPointerChaseLoadStream() const {
    return this->isLoadStream() && this->isPointerChase();
  }

  bool hasCoreUser() const { return !this->coalescedNoCoreUser; }

  /**
   * Get the number of unique cache blocks the stream touches.
   * Used for stream aware cache to determine if it should cache the stream.
   */
  uint64_t getFootprint(unsigned cacheBlockSize) const;
  uint64_t getTrueFootprint() const;
  bool isContinuous() const;
  uint64_t getStreamLengthAtInstance(uint64_t streamInstance) const;

  /**
   * Try to get coalesced offset and size.
   * @return: whether the streamId is coalesced here.
   */
  bool tryGetCoalescedOffsetAndMemSize(uint64_t streamId, int32_t &offset,
                                       int32_t &size) const;
  void getCoalescedOffsetAndMemSize(uint64_t streamId, int32_t &offset,
                                    int32_t &size) const;
  bool isCoalescedHere(uint64_t streamId) const {
    int32_t offset, size;
    return this->tryGetCoalescedOffsetAndMemSize(streamId, offset, size);
  }

  void setNested() { this->nested = true; }
  bool isNestStream() const { return this->nested; }

  bool delayIssueUntilFIFOHead = false;
  void setDelayIssueUntilFIFOHead() { this->delayIssueUntilFIFOHead = true; }
  bool isDelayIssueUntilFIFOHead() const {
    return this->delayIssueUntilFIFOHead;
  }

  bool isNonSpeculative() const { return this->isNonSpec; }

  /**
   * Record the predicated streams from all logical stream.
   */
  std::vector<::LLVM::TDG::PredicatedStreamId> predicatedStreamIds;
  const std::vector<::LLVM::TDG::PredicatedStreamId> &
  getPredicatedStreams() const {
    return predicatedStreamIds;
  }

  using PredFuncInfoVec = std::vector<const ExecFuncInfo *>;
  PredFuncInfoVec predFuncInfos;
  const ExecFuncInfo &getPredicateFuncInfo(PredFuncId predFuncId) const {
    return *this->predFuncInfos.at(predFuncId);
  }
};

struct GetCoalescedStreamValue {
  const Stream *stream;
  const StreamValue streamValue;
  GetCoalescedStreamValue(Stream *_stream, const StreamValue &_streamValue)
      : stream(_stream), streamValue(_streamValue) {}
  StreamValue operator()(uint64_t streamId) const;
};

} // namespace gem5

#endif
