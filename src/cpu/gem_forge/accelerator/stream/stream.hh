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

  uint64_t getStreamId() const { return this->info.id(); }
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
  bool getFloatManual() const {
    return this->info.static_info().float_manual();
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
  const PredicatedStreamIdList &getMergedPredicatedStreams() const {
    return this->info.static_info().merged_predicated_streams();
  }
  const ExecFuncInfo &getPredicateFuncInfo() const {
    return this->info.static_info().pred_func_info();
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
  friend class DynamicStream;
  using StaticId = DynamicStreamId::StaticId;
  using InstanceId = DynamicStreamId::InstanceId;
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
   * Stream initialization is divided into 2 phases:
   * 1. Create the basic unit -- as the place holder.
   * 2. Finalize it.
   *    a. For coalesced stream -- choose the prime logical stream.
   *    b. Find base streams.
   *    c. Find back base streams.
   *    d. Find AliasBaseStream and AliasedStreams.
   * Notice that some information are not valid until finalized, e.g.
   * StreamName, StaticId.
   */
  void addStreamInfo(const LLVM::TDG::StreamInfo &info);
  void finalize();
  void addAddrBaseStream(StaticId baseId, StaticId depId, Stream *baseStream);
  void addValueBaseStream(StaticId baseId, StaticId depId, Stream *baseStream);
  void addBaseStepStream(Stream *baseStepStream);
  void addBackBaseStream(StaticId baseId, StaticId depId,
                         Stream *backBaseStream);
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
  bool isDirectLoadStream() const;
  bool isDirectStoreStream() const;
  bool isIndirectLoadStream() const;
  bool isDirectMemStream() const;
  bool isPointerChaseLoadStream() const { return false; }
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
   * has some DynamicStreams, and the last one has not dispatched
   * the StreamEnd.
   */
  bool isConfigured() const;
  /**
   * Head is the newest element.
   * Tail is the dummy node before the oldest element.
   */
  size_t stepSize;
  size_t maxSize;
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
  struct StreamDepEdge {
    const StaticId fromStaticId = DynamicStreamId::InvalidStaticStreamId;
    const StaticId toStaticId = DynamicStreamId::InvalidStaticStreamId;
    Stream *const toStream = nullptr;
    StreamDepEdge(StaticId _fromId, StaticId _toId, Stream *_toStream)
        : fromStaticId(_fromId), toStaticId(_toId), toStream(_toStream) {}
  };
  using StreamEdges = std::vector<StreamDepEdge>;

  StreamSet addrBaseStreams;
  StreamEdges addrBaseEdges;
  StreamSet addrDepStreams;
  StreamEdges addrDepEdges;

  StreamSet valueBaseStreams;
  StreamEdges valueBaseEdges;
  StreamSet valueDepStreams;
  StreamEdges valueDepEdges;

  /**
   * Back edge dependence on previous iteration.
   */
  StreamSet backBaseStreams;
  StreamEdges backBaseEdges;
  StreamSet backDepStreams;
  StreamEdges backDepEdges;
  bool hasBackDepReductionStream;

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
  StreamStatistic statistic;
  StreamFloatTracer floatTracer;
  void dumpStreamStats(std::ostream &os) const;

  void tick();

  LLVMTraceCPU *getCPU() { return this->cpu; }
  GemForgeCPUDelegator *getCPUDelegator() const;
  int getCPUId() { return this->getCPUDelegator()->cpuId(); }

  void configure(uint64_t seqNum, ThreadContext *tc);

  void dispatchStreamConfig(uint64_t seqNum, ThreadContext *tc);
  void executeStreamConfig(uint64_t seqNum,
                           const DynamicStreamParamV *inputVec);
  void commitStreamConfig(uint64_t seqNum);
  void rewindStreamConfig(uint64_t seqNum);
  bool isStreamConfigureExecuted(uint64_t seqNum);

  void dispatchStreamEnd(uint64_t seqNum);
  void rewindStreamEnd(uint64_t seqNum);
  void commitStreamEnd(uint64_t seqNum);

  /***********************************************************************
   * API to manage the elements of this stream.
   ***********************************************************************/
  /**
   * Get the total number of allocated elements among all dynamic streams.
   */
  int getAllocSize() const { return this->allocSize; }

  /**
   * Allocate a new dynamic instance with FIFOIdx.
   */
  DynamicStreamId allocateNewInstance();

  /**
   * Add value base elements for stream computation.
   */
  void addValueBaseElements(StreamElement *newElement);
  /**
   * Remove one stepped element from the first dynamic stream.
   * @param isEnd: This element is stepped by StreamEnd, not StreamStep.
   */
  StreamElement *releaseElementStepped(bool isEnd);
  /**
   * Remove one unstepped element from the dynamic stream.
   * CommitStreamEnd will release from the first dynamic stream.
   * RewindStreamConfig will release from the last one.
   */
  StreamElement *releaseElementUnstepped(DynamicStream &dynS);
  /**
   * Check if the last dynamic stream has an unstepped element.
   */
  bool hasUnsteppedElement();
  /**
   * Step one element of the last dynamic stream.
   */
  StreamElement *stepElement();
  /**
   * Unstep one element.
   */
  StreamElement *unstepElement();
  /**
   * Get the first unstepped element of the last dynamic stream.
   */
  StreamElement *getFirstUnsteppedElement();
  /**
   * Get the first alive dynamic stream (End not dispatched).
   */
  DynamicStream &getFirstAliveDynStream();
  /**
   * Get the current allocating dynamic stream (may be nullptr).
   * 1. End not dispatched.
   * 2. If has TotalTripCount, it has not reached that limit.
   */
  DynamicStream *getAllocatingDynStream();
  /**
   * Get previous element in the chain of the stream.
   * Notice that it may return nullptr if this is
   * the first element for that stream.
   */
  StreamElement *getPrevElement(StreamElement *element);
  void handleMergedPredicate(const DynamicStream &dynS, StreamElement *element);
  void performStore(const DynamicStream &dynS, StreamElement *element,
                    uint64_t storeValue);

  /**
   * Called by executeStreamConfig() to allow derived class to set up the
   * AddrGenCallback in DynamicStream.
   */
  void setupAddrGen(DynamicStream &dynStream,
                    const DynamicStreamParamV *inputVec);

  /**
   * Extract extra input values from the inputVec. May modify inputVec.
   */
  void extractExtraInputValues(DynamicStream &dynS,
                               DynamicStreamParamV *inputVec);

  /**
   * For debug.
   */
  void dump() const;
  void sampleStatistic();

  /**
   * Allocate the CacheStreamConfigureData.
   */
  CacheStreamConfigureDataPtr
  allocateCacheConfigureData(uint64_t configSeqNum, bool isIndirect = false);

  std::deque<DynamicStream> dynamicStreams;
  bool hasDynamicStream() const { return !this->dynamicStreams.empty(); }
  DynamicStream &getDynamicStream(uint64_t seqNum);
  DynamicStream &getDynamicStreamByEndSeqNum(uint64_t seqNum);
  DynamicStream &getDynamicStreamByInstance(InstanceId instance);
  DynamicStream &getDynamicStreamBefore(uint64_t seqNum);
  DynamicStream *getDynamicStream(const DynamicStreamId &dynId);
  DynamicStream &getLastDynamicStream() {
    assert(!this->dynamicStreams.empty() && "No dynamic stream.");
    return this->dynamicStreams.back();
  }
  DynamicStream &getFirstDynamicStream() {
    assert(!this->dynamicStreams.empty() && "No dynamic stream.");
    return this->dynamicStreams.front();
  }

  LLVMTraceCPU *cpu;
  StreamEngine *se;

  /**
   * StreamAggregateHistory. This is used to detect reuse
   * across stream configuration.
   */
  struct StreamAggregateHistory {
    DynamicStreamFormalParamV addrGenFormalParams;
    uint64_t numReleasedElements = 0;
    uint64_t numIssuedRequests = 0;
    uint64_t numPrivateCacheHits = 0;
    uint64_t startVAddr = 0;
  };
  static constexpr int AggregateHistorySize = 4;
  std::list<StreamAggregateHistory> aggregateHistory;
  void recordAggregateHistory(const DynamicStream &dynS);

  AddrGenCallbackPtr &getAddrGenCallback() { return this->addrGenCallback; }

  std::unique_ptr<StreamAtomicOp>
  setupAtomicOp(FIFOEntryIdx idx, int memElementsize,
                const DynamicStreamFormalParamV &formalParams,
                GetStreamValueFunc getStreamValue);

  bool hasDepNestRegion() const { return this->depNestRegion; }
  void setDepNestRegion() { this->depNestRegion = true; }

protected:
  StreamSet baseStepStreams;
  StreamSet baseStepRootStreams;
  StreamSet dependentStepStreams;

  AddrGenCallbackPtr addrGenCallback;
  ExecFuncPtr predCallback;
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
  void setupLinearAddrFunc(DynamicStream &dynStream,
                           const DynamicStreamParamV *inputVec,
                           const LLVM::TDG::StreamInfo &info);
  /**
   * Helper function to setup an func addr gen.
   */
  void setupFuncAddrFunc(DynamicStream &dynStream,
                         const DynamicStreamParamV *inputVec,
                         const LLVM::TDG::StreamInfo &info);
  /**
   * Helper function to setup formal params for an ExecFunc.
   * @return Number of input value consumed.
   */
  int setupFormalParams(const DynamicStreamParamV *inputVec,
                        const LLVM::TDG::ExecFuncInfo &info,
                        DynamicStreamFormalParamV &formalParams);

  /***************************************************************
   * Managing coalesced LogicalStream within this one.
   * The first one is "prime stream", whose stream id is used to represent
   * this stream.
   ***************************************************************/
  std::vector<LogicalStream *> logicals;
  LogicalStream *primeLogical = nullptr;
  int32_t coalescedElementSize = -1;
  int32_t baseOffset = -1;

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
  Get(bool, FloatManual);
  Get(const PredicatedStreamIdList &, MergedPredicatedStreams);
  Get(const ExecFuncInfo &, PredicateFuncInfo);
  Get(const StreamIdList &, MergedLoadStoreDepStreams);
  Get(const StreamIdList &, MergedLoadStoreBaseStreams);
  Get(const ExecFuncInfo &, StoreFuncInfo);
  Get(const ExecFuncInfo &, LoadFuncInfo);
  Get(bool, EnabledStoreFunc);
  Get(bool, EnabledLoadFunc);
  Is(MergedPredicated);
  Is(MergedLoadStoreDepStream);

  /**
   * Get the coalesce base stream and offset.
   * NOTE: This stream may not be coalesced into the base due to
   * NOTE: large offset (see stream_engine.cc).
   */
  Get(uint64_t, CoalesceBaseStreamId);
  Get(int32_t, CoalesceOffset);

#undef Get
#undef Is

  int32_t getMemElementSize() const {
    assert(this->coalescedElementSize > 0 && "Invalid element size.");
    return this->coalescedElementSize;
  }
  int32_t getCoreElementSize() const {
    if (this->logicals.size() == 1) {
      return this->primeLogical->getCoreElementSize();
    }
    // For coalesced stream CoreElementSize is the same as MemElementSize.
    return this->getMemElementSize();
  }

  bool isMerged() const {
    return this->isMergedPredicated() || this->isMergedLoadStoreDepStream();
  }

  size_t getNumLogicalStreams() const { return this->logicals.size(); }
  bool isSingle() const { return this->getNumLogicalStreams() == 1; }

  bool hasUpdate() const {
    return this->primeLogical->info.static_info()
               .compute_info()
               .update_stream()
               .id() != DynamicStreamId::InvalidStaticStreamId;
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

  bool hasCoreUser() const {
    return !this->primeLogical->info.static_info().no_core_user();
  }

  /**
   * Get the number of unique cache blocks the stream touches.
   * Used for stream aware cache to determine if it should cache the stream.
   */
  uint64_t getFootprint(unsigned cacheBlockSize) const;
  uint64_t getTrueFootprint() const;
  bool isContinuous() const;
  uint64_t getStreamLengthAtInstance(uint64_t streamInstance) const;
  void getCoalescedOffsetAndSize(uint64_t streamId, int32_t &offset,
                                 int32_t &size) const;

  /**
   * Try to get coalesced offset and size.
   * @return: whether the streamId is coalesced here.
   */
  bool tryGetCoalescedOffsetAndSize(uint64_t streamId, int32_t &offset,
                                    int32_t &size) const;
  bool isCoalescedHere(uint64_t streamId) const {
    int32_t offset, size;
    return this->tryGetCoalescedOffsetAndSize(streamId, offset, size);
  }

  void setNested() { this->nested = true; }
  bool isNestStream() const { return this->nested; }
};

#endif
