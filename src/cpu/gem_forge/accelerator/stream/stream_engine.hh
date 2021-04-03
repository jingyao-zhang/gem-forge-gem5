#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_ENGINE_H__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_ENGINE_H__

#include "insts.hh"
#include "prefetch_element_buffer.hh"
#include "stream.hh"
#include "stream_element.hh"
#include "stream_translation_buffer.hh"

#include "stream_float_policy.hh"
#include "stream_placement_manager.hh"

#include "base/statistics.hh"
#include "cpu/gem_forge/accelerator/gem_forge_accelerator.hh"
#include "cpu/gem_forge/lsq.hh"

#include "params/StreamEngine.hh"

#include <unordered_map>

class StreamThrottler;
class StreamLQCallback;
class StreamSQCallback;
class StreamSQDeprecatedCallback;
class StreamFloatController;
class StreamComputeEngine;
class NestStreamController;
class StreamRangeSyncController;

class StreamEngine : public GemForgeAccelerator {
public:
  using Params = StreamEngineParams;
  StreamEngine(Params *params);
  ~StreamEngine() override;

  void handshake(GemForgeCPUDelegator *_cpuDelegator,
                 GemForgeAcceleratorManager *_manager) override;
  void takeOverBy(GemForgeCPUDelegator *newCpuDelegator,
                  GemForgeAcceleratorManager *newManager) override;

  void tick() override;
  void dump() override;
  void regStats() override;
  void resetStats() override;

  // Override the name as we don't want the default long name().
  const std::string name() const override { return "global"; }

  /**
   * To prepare for execution-driven simulation,
   * decouple StreamEngine from StreamInstruction, but
   * use the inst sequence number and protobuf field
   * as the arguments.
   */

  struct StreamConfigArgs {
    using InputVec = DynamicStreamParamV;
    using InputMap = std::unordered_map<uint64_t, InputVec>;
    uint64_t seqNum; // Just the instruction sequence number.
    const std::string &infoRelativePath; // Where to find the info.
    const InputMap *inputMap;            // Live input of streams.
    // Only valid at dispatchStreamConfig for execution simulation.
    ThreadContext *const tc;
    StreamConfigArgs(uint64_t _seqNum, const std::string &_infoRelativePath,
                     InputMap *_inputMap = nullptr,
                     ThreadContext *_tc = nullptr)
        : seqNum(_seqNum), infoRelativePath(_infoRelativePath),
          inputMap(_inputMap), tc(_tc) {}
  };

  bool canStreamConfig(const StreamConfigArgs &args) const;
  void dispatchStreamConfig(const StreamConfigArgs &args);
  void executeStreamConfig(const StreamConfigArgs &args);
  void commitStreamConfig(const StreamConfigArgs &args);
  void rewindStreamConfig(const StreamConfigArgs &args);

  bool canDispatchStreamStep(uint64_t stepStreamId) const;
  void dispatchStreamStep(uint64_t stepStreamId);
  bool canCommitStreamStep(uint64_t stepStreamId);
  void commitStreamStep(uint64_t stepStreamId);
  void rewindStreamStep(uint64_t stepStreamId);

  struct StreamUserArgs {
    static constexpr int MaxElementSize = 64;
    using Value = std::array<uint8_t, MaxElementSize>;
    using ValueVec = std::vector<Value>;
    uint64_t seqNum;
    Addr pc;
    const std::vector<uint64_t> &usedStreamIds;
    // Is this a StreamStore (to distinguish for UpdateStream).
    bool isStore = false;
    // Used to return the stream values.
    // Only used in executeStreamUser().
    ValueVec *values;
    StreamUserArgs(uint64_t _seqNum, Addr _pc,
                   const std::vector<uint64_t> &_usedStreamIds,
                   bool _isStore = false, ValueVec *_values = nullptr)
        : seqNum(_seqNum), pc(_pc), usedStreamIds(_usedStreamIds),
          isStore(_isStore), values(_values) {}
  };

  int getStreamUserLQEntries(const StreamUserArgs &args) const;
  /**
   * Create LSQ callbacks for first stream user.
   * @return The number of LSQ callbacks created.
   */
  int createStreamUserLSQCallbacks(const StreamUserArgs &args,
                                   GemForgeLSQCallbackList &callbacks);

  bool hasUnsteppedElement(const StreamUserArgs &args);
  bool hasIllegalUsedLastElement(const StreamUserArgs &args);
  bool canDispatchStreamUser(const StreamUserArgs &args);
  void dispatchStreamUser(const StreamUserArgs &args);
  bool areUsedStreamsReady(const StreamUserArgs &args);
  void executeStreamUser(const StreamUserArgs &args);
  void commitStreamUser(const StreamUserArgs &args);
  void rewindStreamUser(const StreamUserArgs &args);

  struct StreamEndArgs {
    uint64_t seqNum;
    const std::string &infoRelativePath;
    StreamEndArgs(uint64_t _seqNum, const std::string &_infoRelativePath)
        : seqNum(_seqNum), infoRelativePath(_infoRelativePath) {}
  };
  bool hasUnsteppedElement(const StreamEndArgs &args);
  void dispatchStreamEnd(const StreamEndArgs &args);
  bool canExecuteStreamEnd(const StreamEndArgs &args);
  bool canCommitStreamEnd(const StreamEndArgs &args);
  void commitStreamEnd(const StreamEndArgs &args);
  void rewindStreamEnd(const StreamEndArgs &args);

  bool canStreamStoreDispatch(const StreamStoreInst *inst) const;
  std::list<std::unique_ptr<GemForgeSQDeprecatedCallback>>
  createStreamStoreSQCallbacks(StreamStoreInst *inst);
  void dispatchStreamStore(StreamStoreInst *inst);
  void executeStreamStore(StreamStoreInst *inst);
  void commitStreamStore(StreamStoreInst *inst);

  /*************************************************************************
   * Tough part: handling misspeculation.
   ************************************************************************/
  void cpuStoreTo(InstSeqNum seqNum, Addr vaddr, int size);
  void addPendingWritebackElement(StreamElement *releaseElement);

  Stream *getStream(uint64_t streamId) const;
  Stream *tryGetStream(uint64_t streamId) const;

  /**
   * Send StreamEnd packet for all the ended dynamic stream ids.
   */
  void sendStreamFloatEndPacket(const std::vector<DynamicStreamId> &endedIds);

  /**
   * Send atomic operation packet.
   */
  void sendAtomicPacket(StreamElement *element, AtomicOpFunctorPtr atomicOp);

  StreamPlacementManager *getStreamPlacementManager() {
    return this->streamPlacementManager;
  }

  /**
   * Used by StreamPlacementManager to get all cache.
   */
  std::vector<SimObject *> &getSimObjectList() {
    return SimObject::getSimObjectList();
  }

  bool isTraceSim() const {
    assert(cpuDelegator && "Missing cpuDelegator.");
    return cpuDelegator->cpuType == GemForgeCPUDelegator::CPUTypeE::LLVM_TRACE;
  }

  bool isMergeEnabled() const { return this->enableMerge; }
  bool isOracleEnabled() const { return this->isOracle; }

  bool isPlacementEnabled() const { return this->enableStreamPlacement; }
  bool isPlacementBusEnabled() const { return this->enableStreamPlacementBus; }
  bool isPlacementNoBypassingStore() const { return this->noBypassingStore; }
  bool isContinuousStoreOptimized() const { return this->continuousStore; }
  bool isPlacementPeriodReset() const {
    return this->enablePlacementPeriodReset;
  }
  bool isOraclePlacementEnabled() const {
    return this->enableStreamPlacementOracle;
  }
  bool isStreamFloatCancelEnabled() const {
    return this->enableStreamFloatCancel;
  }
  bool isStreamRangeSyncEnabled() const {
    return this->myParams->enableRangeSync;
  }
  const std::string &getPlacementLat() const { return this->placementLat; }
  const std::string &getPlacement() const { return this->placement; }

  void exitDump() const;

  void fetchedCacheBlock(Addr cacheBlockVAddr, StreamMemAccess *memAccess);
  void incrementInflyStreamRequest() { this->numInflyStreamRequests++; }
  void decrementInflyStreamRequest() {
    assert(this->numInflyStreamRequests > 0);
    this->numInflyStreamRequests--;
  }

  int currentTotalRunAheadLength;
  int totalRunAheadLength;
  int totalRunAheadBytes;

  /**
   * Stats
   */
  mutable Stats::Scalar numConfigured;
  mutable Stats::Scalar numStepped;
  mutable Stats::Scalar numUnstepped;
  mutable Stats::Scalar numElementsAllocated;
  mutable Stats::Scalar numElementsUsed;
  mutable Stats::Scalar numCommittedStreamUser;
  mutable Stats::Scalar entryWaitCycles;

  mutable Stats::Scalar numStoreElementsAllocated;
  mutable Stats::Scalar numStoreElementsStepped;
  mutable Stats::Scalar numStoreElementsUsed;

  mutable Stats::Scalar numLoadElementsAllocated;
  mutable Stats::Scalar numLoadElementsFetched;
  mutable Stats::Scalar numLoadElementsStepped;
  mutable Stats::Scalar numLoadElementsUsed;
  mutable Stats::Scalar numLoadElementWaitCycles;
  mutable Stats::Scalar numLoadCacheLineFetched;
  mutable Stats::Scalar numLoadCacheLineUsed;
  /**
   * * How many times a StreamUser/StreamStore not dispatched due to LSQ full.
   */
  mutable Stats::Scalar streamUserNotDispatchedByLoadQueue;
  mutable Stats::Scalar streamStoreNotDispatchedByStoreQueue;

  Stats::Distribution numTotalAliveElements;
  Stats::Distribution numTotalAliveCacheBlocks;
  Stats::Distribution numRunAHeadLengthDist;
  Stats::Distribution numTotalAliveMemStreams;
  Stats::Distribution numInflyStreamRequestDist;

  /**
   * Statistics for stream placement manager.
   */
  Stats::Vector numAccessPlacedInCacheLevel;
  Stats::Vector numAccessHitHigherThanPlacedCacheLevel;
  Stats::Vector numAccessHitLowerThanPlacedCacheLevel;

  Stats::Distribution numAccessFootprintL1;
  Stats::Distribution numAccessFootprintL2;
  Stats::Distribution numAccessFootprintL3;

  /**
   * Statistics for stream float.
   */
  Stats::Scalar numFloated;
  Stats::Scalar numLLCSentSlice;
  Stats::Scalar numLLCMigrated;
  Stats::Scalar numMLCResponse;

  /**
   * Statistics for stream computing.
   */
  Stats::Scalar numScheduledComputation;
  Stats::Scalar numCompletedComputation;
  Stats::Scalar numCompletedComputeMicroOps;

private:
  friend class Stream;
  friend class StreamElement;
  friend class StreamThrottler;
  friend class StreamLQCallback;
  friend class StreamSQCallback;
  friend class StreamSQDeprecatedCallback;
  friend class StreamFloatController;
  friend class NestStreamController;
  friend class StreamRangeSyncController;

  LLVMTraceCPU *cpu;

  std::unique_ptr<StreamTranslationBuffer<void *>> translationBuffer;
  StreamPlacementManager *streamPlacementManager;

  std::vector<StreamElement> FIFOArray;
  StreamElement *FIFOFreeListHead;
  size_t numFreeFIFOEntries;

  /**
   * Incremented when dispatch StreamConfig and decremented when commit
   * StreamEnd.
   */
  int numInflyStreamConfigurations = 0;

  /**
   * Incremented when issue a stream request out and decremented when the
   * result comes back.
   */
  int numInflyStreamRequests = 0;

  /**
   * Map from the user instruction seqNum to all the actual element to use.
   * Update at dispatchStreamUser and commitStreamUser.
   */
  std::unordered_map<uint64_t, std::unordered_set<StreamElement *>>
      userElementMap;

  /**
   * Map from the stream element to the user instruction.
   * A reverse map of userElementMap.
   */
  std::unordered_map<StreamElement *, std::unordered_set<uint64_t>>
      elementUserMap;

  /**
   * Holds the prefetch element buffer.
   */
  PrefetchElementBuffer peb;

  using StreamId = uint64_t;
  std::unordered_map<StreamId, Stream *> streamMap;

  /**
   * One level indirection for from the original stream id to coalesced stream
   * id. This is to make sure that streamMap always maintains an one-to-one
   * mapping.
   */
  std::unordered_map<StreamId, StreamId> coalescedStreamIdMap;

  /**
   * Flags.
   */
  Params *myParams;
  bool isOracle;
  unsigned defaultRunAheadLength;
  bool enableLSQ;
  bool enableCoalesce;
  bool enableMerge;
  bool enableStreamPlacement;
  bool enableStreamPlacementOracle;
  bool enableStreamPlacementBus;
  bool enablePlacementPeriodReset;
  bool noBypassingStore;
  bool continuousStore;
  bool enableStreamFloat;
  bool enableStreamFloatIndirect;
  bool enableStreamFloatPseudo;
  bool enableStreamFloatCancel;
  std::string placementLat;
  std::string placement;
  /**
   * A dummy cacheline of data for write back.
   */
  uint8_t *writebackCacheLine;

  /**
   * ! This is a hack to avoid having alias for IndirectUpdateStream.
   * The DynStream tracks writes from this stream that has not been writen back.
   * And delay issuing new elements if found aliased here.
   */
  struct PendingWritebackElement {
    FIFOEntryIdx fifoIdx;
    Addr vaddr;
    int size;
    PendingWritebackElement(FIFOEntryIdx _fifoIdx, Addr _vaddr, int _size)
        : fifoIdx(_fifoIdx), vaddr(_vaddr), size(_size) {}
  };
  std::map<InstSeqNum, PendingWritebackElement> pendingWritebackElements;
  /**
   * Check if this access aliased with my pending writeback elements.
   */
  bool hasAliasWithPendingWritebackElements(StreamElement *checkElement,
                                            Addr vaddr, int size) const;
  void removePendingWritebackElement(InstSeqNum seqNum, Addr vaddr, int size);

  /**
   * Memorize the StreamConfigureInfo.
   */
  mutable std::unordered_map<std::string, ::LLVM::TDG::StreamRegion>
      memorizedStreamRegionMap;

  struct CacheBlockInfo {
    int reference;
    enum Status {
      INIT,
      FETCHING,
      FETCHED,
    };
    Status status;
    bool used;
    bool requestedByLoad;
    std::list<StreamMemAccess *> pendingAccesses;
    CacheBlockInfo()
        : reference(0), status(Status::INIT), used(false),
          requestedByLoad(false) {}
  };
  std::unordered_map<Addr, CacheBlockInfo> cacheBlockRefMap;

  void initializeStreams(const ::LLVM::TDG::StreamRegion &streamRegion);
  void
  generateCoalescedStreamIdMap(const ::LLVM::TDG::StreamRegion &streamRegion,
                               Stream::StreamArguments &args);

  Stream *getOrInitializeStream(uint64_t stepRootStreamId,
                                int32_t coalesceGroup);

  void updateAliveStatistics();

  void initializeFIFO(size_t totalElements);
  void addFreeElement(StreamElement *S);
  StreamElement *removeFreeElement();
  bool hasFreeElement() const;

  // Memorize the step stream list.
  mutable std::unordered_map<Stream *, std::list<Stream *>>
      memorizedStreamStepListMap;
  const std::list<Stream *> &getStepStreamList(Stream *stepS) const;

  // Helper function to get streams configured in the region.
  mutable std::unordered_map<const LLVM::TDG::StreamRegion *,
                             std::list<Stream *>>
      memorizedRegionConfiguredStreamsMap;
  const std::list<Stream *> &
  getConfigStreamsInRegion(const LLVM::TDG::StreamRegion &streamRegion);

  // Called every cycle to allocate elements.
  void allocateElements();
  // Allocate one element to stream.
  void allocateElement(DynamicStream &dynS);
  /**
   * Release an unstepped stream element.
   * Used to clear ended stream.
   */
  bool releaseElementUnstepped(DynamicStream &dynS);
  /**
   * Release a stepped stream element.
   * @param isEnd: this element is stepped by StreamEnd, not StreamStep.
   * @param toThrottle: perform stream throttling.
   */
  void releaseElementStepped(Stream *S, bool isEnd, bool doThrottle);
  void issueElements();
  void issueElement(StreamElement *element);
  void prefetchElement(StreamElement *element);
  void writebackElement(StreamElement *element, StreamStoreInst *inst);

  /**
   * Flush the PEB entries.
   */
  void flushPEB(Addr vaddr, int size);

  /**
   * LoadQueue RAW misspeculation.
   */
  void RAWMisspeculate(StreamElement *element);

  std::vector<StreamElement *> findReadyElements();

  size_t getTotalRunAheadLength() const;

  const ::LLVM::TDG::StreamRegion &
  getStreamRegion(const std::string &relativePath) const;

  void dumpFIFO() const;
  void dumpUser() const;

  std::unique_ptr<StreamThrottler> throttler;
  std::unique_ptr<StreamFloatController> floatController;
  std::unique_ptr<StreamComputeEngine> computeEngine;
  std::unique_ptr<NestStreamController> nestStreamController;
  std::unique_ptr<StreamRangeSyncController> rangeSyncController;

  /**
   * Try to coalesce continuous element of direct mem stream if they
   * overlap.
   */
  void coalesceContinuousDirectMemStreamElement(StreamElement *element);
};

#endif
