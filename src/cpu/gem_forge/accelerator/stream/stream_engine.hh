#ifndef __CPU_TDG_ACCELERATOR_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_STREAM_ENGINE_H__

#include "coalesced_stream.hh"
#include "insts.hh"
#include "single_stream.hh"
#include "stream_element.hh"

#include "stream_placement_manager.hh"

#include "base/statistics.hh"
#include "cpu/gem_forge/accelerator/tdg_accelerator.hh"

#include <unordered_map>

class StreamEngine : public TDGAccelerator {
public:
  StreamEngine();
  ~StreamEngine() override;

  void handshake(LLVMTraceCPU *_cpu, TDGAcceleratorManager *_manager) override;

  bool handle(LLVMDynamicInst *inst) override;
  void tick() override;
  void dump() override;
  void regStats() override;

  bool canStreamConfig(const StreamConfigInst *inst) const;
  void dispatchStreamConfigure(StreamConfigInst *inst);
  void commitStreamConfigure(StreamConfigInst *inst);

  bool canStreamStep(const StreamStepInst *inst) const;
  void dispatchStreamStep(StreamStepInst *inst);
  void commitStreamStep(StreamStepInst *inst);

  bool canStreamUserDispatch(const LLVMDynamicInst *inst) const;
  void dispatchStreamUser(LLVMDynamicInst *inst);
  bool areUsedStreamsReady(const LLVMDynamicInst *inst);
  void executeStreamUser(LLVMDynamicInst *inst);
  void commitStreamUser(LLVMDynamicInst *inst);

  void dispatchStreamEnd(StreamEndInst *inst);
  void commitStreamEnd(StreamEndInst *inst);

  bool canStreamStoreDispatch(const StreamStoreInst *inst) const;
  void dispatchStreamStore(StreamStoreInst *inst);
  void executeStreamStore(StreamStoreInst *inst);
  void commitStreamStore(StreamStoreInst *inst);

  Stream *getStream(uint64_t streamId) const;

  StreamPlacementManager *getStreamPlacementManager() {
    return this->streamPlacementManager;
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
  const std::string &getPlacementLat() const { return this->placementLat; }
  const std::string &getPlacement() const { return this->placement; }

  void exitDump() const {
    if (streamPlacementManager != nullptr) {
      this->streamPlacementManager->dumpStreamCacheStats();
    }
  }

  void fetchedCacheBlock(Addr cacheBlockVirtualAddr,
                         StreamMemAccess *memAccess);

  int currentTotalRunAheadLength;
  int maxTotalRunAheadLength;

  /**
   * Stats
   */
  mutable Stats::Scalar numConfigured;
  mutable Stats::Scalar numStepped;
  mutable Stats::Scalar numStreamMemRequests;
  mutable Stats::Scalar numElements;
  mutable Stats::Scalar numElementsUsed;
  mutable Stats::Scalar numUnconfiguredStreamUse;
  mutable Stats::Scalar numConfiguredStreamUse;
  mutable Stats::Scalar entryWaitCycles;
  mutable Stats::Scalar numMemElements;
  mutable Stats::Scalar numMemElementsFetched;
  mutable Stats::Scalar numMemElementsUsed;
  mutable Stats::Scalar memEntryWaitCycles;
  mutable Stats::Scalar streamUserNotDispatchedByLoadQueue;
  mutable Stats::Scalar streamStoreNotDispatchedByStoreQueue;

  Stats::Distribution numTotalAliveElements;
  Stats::Distribution numTotalAliveCacheBlocks;
  Stats::Distribution numRunAHeadLengthDist;
  Stats::Distribution numTotalAliveMemStreams;

  /**
   * Statistics for stream placement manager.
   */
  Stats::Vector numAccessPlacedInCacheLevel;
  Stats::Vector numAccessHitHigherThanPlacedCacheLevel;
  Stats::Vector numAccessHitLowerThanPlacedCacheLevel;

  Stats::Distribution numAccessFootprintL1;
  Stats::Distribution numAccessFootprintL2;
  Stats::Distribution numAccessFootprintL3;

private:
  StreamPlacementManager *streamPlacementManager;

  std::vector<StreamElement> FIFOArray;
  StreamElement *FIFOFreeListHead;
  size_t numFreeFIFOEntries;

  /**
   * Map from the user instruction to all the actual element to use.
   * Update at dispatchStreamUser and commitStreamUser.
   */
  std::unordered_map<const LLVMDynamicInst *,
                     std::unordered_set<StreamElement *>>
      userElementMap;

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
  bool isOracle;
  unsigned maxRunAHeadLength;
  enum ThrottlingStrategyE {
    STATIC,
    DYNAMIC,
    GLOBAL,
  };
  ThrottlingStrategyE throttlingStrategy;
  bool enableLSQ;
  bool enableCoalesce;
  bool enableMerge;
  bool enableStreamPlacement;
  bool enableStreamPlacementOracle;
  bool enableStreamPlacementBus;
  bool enablePlacementPeriodReset;
  bool noBypassingStore;
  bool continuousStore;
  std::string placementLat;
  std::string placement;

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
    std::list<StreamMemAccess *> pendingAccesses;
    CacheBlockInfo() : reference(0), status(Status::INIT) {}
  };
  std::unordered_map<Addr, CacheBlockInfo> cacheBlockRefMap;

  void initializeStreams(const ::LLVM::TDG::StreamRegion &streamRegion);

  CoalescedStream *getOrInitializeCoalescedStream(uint64_t stepRootStreamId,
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

  // Allocate one element to stream.
  bool areBaseElementAllocated(Stream *S);
  void allocateElement(Stream *S);
  void releaseElement(Stream *S);
  void issueElements();
  void issueElement(StreamElement *element);
  void throttleStream(Stream *S, StreamElement *element);

  size_t getTotalRunAheadLength() const;

  const ::LLVM::TDG::StreamRegion &
  getStreamRegion(const std::string &relativePath) const;

  void dumpFIFO() const;

  /**
   * Helper class to throttle the stream's maxSize.
   */
  class StreamThrottler {
  public:
    StreamEngine *se;
    StreamThrottler(StreamEngine *_se);

    void throttleStream(Stream *S, StreamElement *element);
  };

  StreamThrottler throttler;
};

#endif
