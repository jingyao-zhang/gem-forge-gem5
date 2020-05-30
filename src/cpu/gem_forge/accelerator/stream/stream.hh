#ifndef __GEM_FORGE_ACCELERATOR_STREAM_HH__
#define __GEM_FORGE_ACCELERATOR_STREAM_HH__

#include "cache/CacheStreamConfigureData.hh"
#include "cpu/gem_forge/llvm_insts.hh"
#include "dyn_stream.hh"
#include "stream_element.hh"
#include "stream_float_tracer.hh"
#include "stream_statistic.hh"

#include "base/types.hh"
#include "mem/packet.hh"

#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif

#include "StreamMessage.pb.h"

#include <list>

class LLVMTraceCPU;
class GemForgeCPUDelegator;

class StreamEngine;
class StreamConfigInst;
class StreamEndInst;

/**
 * Holdes the aggregated stream state, across multiple dynamic streams.
 */
class Stream {
public:
  struct StreamArguments {
    LLVMTraceCPU *cpu;
    GemForgeCPUDelegator *cpuDelegator;
    StreamEngine *se;
    int maxSize;
    const ::LLVM::TDG::StreamRegion *streamRegion;
    uint64_t staticId;
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
  virtual void finalize() = 0;
  void addBaseStream(Stream *baseStream);
  void addBaseStepStream(Stream *baseStepStream);
  void addBackBaseStream(Stream *backBaseStream);
  void registerStepDependentStreamToRoot(Stream *newDependentStream);
  void
  initializeAliasStreamsFromProtobuf(const ::LLVM::TDG::StaticStreamInfo &info);
  void initializeCoalesceGroupStreams();

  const std::string &getStreamName() const { return this->streamName; }
  virtual ::LLVM::TDG::StreamInfo_Type getStreamType() const = 0;
  bool isMemStream() const;
  virtual uint32_t getLoopLevel() const = 0;
  virtual uint32_t getConfigLoopLevel() const = 0;
  virtual int32_t getElementSize() const = 0;
  virtual bool getFloatManual() const = 0;

  virtual bool hasUpdate() const = 0;
  virtual bool hasUpgradedToUpdate() const = 0;

  virtual bool isReduction() const = 0;
  virtual bool hasCoreUser() const = 0;
  /**
   * Whether this stream has been merged, including predicated merge.
   */
  using PredicatedStreamIdList =
      ::google::protobuf::RepeatedPtrField<::LLVM::TDG::PredicatedStreamId>;
  using StreamIdList =
      ::google::protobuf::RepeatedPtrField<::LLVM::TDG::StreamId>;
  virtual const PredicatedStreamIdList &getMergedPredicatedStreams() const = 0;
  virtual const ::LLVM::TDG::ExecFuncInfo &getPredicateFuncInfo() const = 0;
  virtual const StreamIdList &getMergedLoadStoreDepStreams() const = 0;
  virtual const StreamIdList &getMergedLoadStoreBaseStreams() const = 0;
  virtual const ::LLVM::TDG::ExecFuncInfo &getStoreFuncInfo() const = 0;
  virtual bool isMerged() const {
    return this->isMergedPredicated() || this->isMergedLoadStoreDepStream();
  }
  virtual bool isMergedPredicated() const = 0;
  virtual bool isMergedLoadStoreDepStream() const = 0;
  virtual bool enabledStoreFunc() const = 0;
  virtual const ::LLVM::TDG::StreamParam &getConstUpdateParam() const = 0;
  /**
   * Get coalesce base stream, 0 for invalid.
   */
  virtual uint64_t getCoalesceBaseStreamId() const { return 0; }
  virtual int32_t getCoalesceOffset() const { return 0; }

  /**
   * Simple bookkeeping information for the stream engine.
   */
  bool configured;
  /**
   * Head is the newest element.
   * Tail is the dummy node before the oldest element.
   */
  size_t stepSize;
  size_t maxSize;
  FIFOEntryIdx FIFOIdx;
  int lateFetchCount;
  int numInflyStreamRequests = 0;
  void incrementInflyStreamRequest() { this->numInflyStreamRequests++; }
  void decrementInflyStreamRequest() {
    assert(this->numInflyStreamRequests > 0);
    this->numInflyStreamRequests--;
  }

  const ::LLVM::TDG::StreamRegion *streamRegion;
  uint64_t staticId;
  std::string streamName;

  /**
   * Step root stream, three possible cases:
   * 1. this: I am the step root.
   * 2. other: I am controlled by other step stream.
   * 3. nullptr: I am a constant stream.
   */
  Stream *stepRootStream;
  using StreamSet = std::unordered_set<Stream *>;
  using StreamVec = std::vector<Stream *>;
  StreamSet baseStreams;
  StreamSet dependentStreams;
  /**
   * Back edge dependence on previous iteration.
   */
  StreamSet backBaseStreams;
  StreamSet backDependentStreams;
  bool hasBackDepReductionStream;
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

  virtual uint64_t getTrueFootprint() const = 0;
  virtual uint64_t getFootprint(unsigned cacheBlockSize) const = 0;
  virtual bool isContinuous() const = 0;

  LLVMTraceCPU *getCPU() { return this->cpu; }
  int getCPUId() { return this->cpuDelegator->cpuId(); }
  GemForgeCPUDelegator *getCPUDelegator() { return this->cpuDelegator; }

  virtual void configure(uint64_t seqNum, ThreadContext *tc) = 0;

  using InputVecT = std::vector<DynamicStream::StreamValueT>;
  void dispatchStreamConfig(uint64_t seqNum, ThreadContext *tc);
  void executeStreamConfig(uint64_t seqNum, const InputVecT *inputVec);
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
   * Add one element to the last dynamic stream.
   */
  void allocateElement(StreamElement *newElement);
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
   * Check if the last dynamic stream can be stepped.
   */
  bool canStep() const { return this->allocSize - this->stepSize >= 2; }
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
   * Get previous element in the chain of the stream.
   * Notice that it may return the (dummy) element->stream->tail if this is
   * the first element for that stream.
   */
  StreamElement *getPrevElement(StreamElement *element);
  /**
   * Perform const update for stepped && used element.
   */
  void handleConstUpdate(const DynamicStream &dynS, StreamElement *element);
  void handleStoreFunc(const DynamicStream &dynS, StreamElement *element);
  void handleMergedPredicate(const DynamicStream &dynS, StreamElement *element);
  void performConstStore(const DynamicStream &dynS, StreamElement *element);

  /**
   * Called by executeStreamConfig() to allow derived class to set up the
   * AddrGenCallback in DynamicStream.
   */
  virtual void setupAddrGen(DynamicStream &dynStream,
                            const InputVecT *inputVec) = 0;

  /**
   * Extract extra input values from the inputVec. May modify inputVec.
   */
  void extractExtraInputValues(DynamicStream &dynS, InputVecT &inputVec);

  /**
   * For debug.
   */
  void dump() const;

  /**
   * ! Sean: StreamAwareCache
   * Allocate the CacheStreamConfigureData.
   */
  CacheStreamConfigureData *allocateCacheConfigureData(uint64_t configSeqNum,
                                                       bool isIndirect = false);

  /**
   * Helper function used in StreamAwareCache.
   */
  bool isDirectLoadStream() const;
  bool isDirectMemStream() const;
  virtual bool isPointerChaseLoadStream() const { return false; }
  virtual uint64_t getStreamLengthAtInstance(uint64_t streamInstance) const = 0;

  std::deque<DynamicStream> dynamicStreams;
  bool hasDynamicStream() const { return !this->dynamicStreams.empty(); }
  DynamicStream &getDynamicStream(uint64_t seqNum);
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
  GemForgeCPUDelegator *cpuDelegator;
  StreamEngine *se;

  /**
   * StreamAggregateHistory. This is used to detect reuse
   * across stream configuration.
   */
  struct StreamAggregateHistory {
    DynamicStreamFormalParamV addrGenFormalParams;
    uint64_t numReleasedElements;
    uint64_t numIssuedRequests;
  };
  static constexpr int AggregateHistorySize = 4;
  std::list<StreamAggregateHistory> aggregateHistory;
  void recordAggregateHistory(const DynamicStream &dynS);

  AddrGenCallbackPtr &getAddrGenCallback() { return this->addrGenCallback; }

  DynamicStreamParamV
  setupAtomicRMWParamV(const DynamicStreamFormalParamV formalParams) const;

protected:
  StreamSet baseStepStreams;
  StreamSet baseStepRootStreams;
  StreamSet dependentStepStreams;

  AddrGenCallbackPtr addrGenCallback;
  ExecFuncPtr predCallback;
  ExecFuncPtr storeCallback;

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
  void setupLinearAddrFunc(DynamicStream &dynStream, const InputVecT *inputVec,
                           const LLVM::TDG::StreamInfo &info);
  /**
   * Helper function to setup an func addr gen.
   */
  void setupFuncAddrFunc(DynamicStream &dynStream, const InputVecT *inputVec,
                         const LLVM::TDG::StreamInfo &info);
  /**
   * Helper function to setup formal params for an ExecFunc.
   * @return Number of input value consumed.
   */
  int setupFormalParams(const InputVecT *inputVec,
                        const LLVM::TDG::ExecFuncInfo &info,
                        DynamicStreamFormalParamV &formalParams);
};

#endif
