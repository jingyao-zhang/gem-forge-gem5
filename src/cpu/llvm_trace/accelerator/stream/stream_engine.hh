#ifndef __CPU_TDG_ACCELERATOR_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_STREAM_ENGINE_H__

#include "coalesced_stream.hh"
#include "insts.hh"
#include "single_stream.hh"
#include "stream_element.hh"

#include "stream_placement_manager.hh"

#include "base/statistics.hh"
#include "cpu/llvm_trace/accelerator/tdg_accelerator.hh"

#include <unordered_map>

class StreamEngine : public TDGAccelerator {
public:
  StreamEngine();
  ~StreamEngine() override;

  void handshake(LLVMTraceCPU *_cpu, TDGAcceleratorManager *_manager) override;
  void setIsOracle(bool isOracle) { this->isOracle = isOracle; }

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

  void dispatchStreamUser(LLVMDynamicInst *inst);
  bool areUsedStreamsReady(const LLVMDynamicInst *inst);
  void executeStreamUser(LLVMDynamicInst *inst);
  void commitStreamUser(LLVMDynamicInst *inst);

  void dispatchStreamEnd(StreamEndInst *inst);
  void commitStreamEnd(StreamEndInst *inst);

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
  Stats::Scalar numConfigured;
  Stats::Scalar numStepped;
  Stats::Scalar numStreamMemRequests;
  Stats::Scalar numElements;
  Stats::Scalar numElementsUsed;
  Stats::Scalar numUnconfiguredStreamUse;
  Stats::Scalar numConfiguredStreamUse;
  Stats::Scalar entryWaitCycles;
  Stats::Scalar numMemElements;
  Stats::Scalar numMemElementsFetched;
  Stats::Scalar numMemElementsUsed;
  Stats::Scalar memEntryWaitCycles;

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
  // std::unordered_map<uint64_t, std::unordered_map<uint64_t, CoalescedStream>>
  // coalescedStreamMap;

  /**
   * Flags.
   */
  bool isOracle;
  unsigned maxRunAHeadLength;
  enum ThrottlingE {
    STATIC,
    DYNAMIC,
  };
  ThrottlingE throttling;
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

  void initializeStreams(
      const LLVM::TDG::TDGInstruction::StreamConfigExtra &configExtra);

  CoalescedStream *getOrInitializeCoalescedStream(uint64_t stepRootStreamId,
                                                  int32_t coalesceGroup);

  void updateAliveStatistics();

  // A helper function to load a stream info protobuf file.
  static LLVM::TDG::StreamInfo
  parseStreamInfoFromFile(const std::string &infoPath);

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

  void dumpFIFO() const;
};

#endif
