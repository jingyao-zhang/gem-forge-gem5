#ifndef __CPU_TDG_ACCELERATOR_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_STREAM_ENGINE_H__

#include "coalesced_stream.hh"
#include "insts.hh"
#include "single_stream.hh"

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

  void useStream(uint64_t streamId, const LLVMDynamicInst *user);
  bool isStreamReady(uint64_t streamId, const LLVMDynamicInst *user) const;
  bool canStreamStep(uint64_t streamId) const;
  void commitStreamConfigure(StreamConfigInst *inst);
  void commitStreamStep(StreamStepInst *inst);
  void commitStreamStore(StreamStoreInst *inst);
  void commitStreamEnd(StreamEndInst *inst);
  void commitStreamUser(uint64_t streamId, const LLVMDynamicInst *user);

  const Stream *getStreamNullable(uint64_t streamId) const;
  Stream *getStreamNullable(uint64_t streamId);

  StreamPlacementManager *getStreamPlacementManager() {
    return this->streamPlacementManager;
  }

  bool isMergeEnabled() const { return this->enableMerge; }
  bool isOracleEnabled() const { return this->isOracle; }

  bool isPlacementEnabled() const { return this->enableStreamPlacement; }
  bool isPlacementBusEnabled() const { return this->enableStreamPlacementBus; }
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
  Stats::Scalar numCacheLevel;
  Stats::Distribution numAccessPlacedInCacheLevel;
  Stats::Distribution numAccessHitHigherThanPlacedCacheLevel;
  Stats::Distribution numAccessHitLowerThanPlacedCacheLevel;

  Stats::Distribution numAccessFootprintL1;
  Stats::Distribution numAccessFootprintL2;
  Stats::Distribution numAccessFootprintL3;

private:
  StreamPlacementManager *streamPlacementManager;

  std::unordered_map<uint64_t, Stream *> streamMap;

  /**
   * Map of the CoalescedStream.
   * Indexed by the <StepRootStreamId, CoalescedGroupId>
   */
  std::unordered_map<uint64_t, std::unordered_map<uint64_t, CoalescedStream>>
      coalescedStreamMap;

  /**
   * Flags.
   */
  bool isOracle;
  unsigned maxRunAHeadLength;
  std::string throttling;
  bool enableCoalesce;
  bool enableMerge;
  bool enableStreamPlacement;
  bool enableStreamPlacementOracle;
  bool enableStreamPlacementBus;
  std::string placementLat;
  std::string placement;

  Stream *getOrInitializeStream(
      const LLVM::TDG::TDGInstruction_StreamConfigExtra &configInst);

  CoalescedStream *getOrInitializeCoalescedStream(uint64_t stepRootStreamId,
                                                  int32_t coalesceGroup);

  void updateAliveStatistics();

  // A helper function to load a stream info protobuf file.
  static LLVM::TDG::StreamInfo
  parseStreamInfoFromFile(const std::string &infoPath);
};

#endif
