#ifndef __CPU_TDG_ACCELERATOR_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_STREAM_ENGINE_H__

#include "coalesced_stream.hh"
#include "insts.hh"
#include "single_stream.hh"

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
  void regStats() override;

  void useStream(uint64_t streamId, const LLVMDynamicInst *user);
  bool isStreamReady(uint64_t streamId, const LLVMDynamicInst *user) const;
  bool canStreamStep(uint64_t streamId) const;
  void commitStreamConfigure(StreamConfigInst *inst);
  void commitStreamStep(StreamStepInst *inst);
  void commitStreamStore(StreamStoreInst *inst);
  void commitStreamEnd(StreamEndInst *inst);

  const Stream *getStreamNullable(uint64_t streamId) const;
  Stream *getStreamNullable(uint64_t streamId);

  bool isMergeEnabled() const { return this->enableMerge; }

  /**
   * Stats
   */
  Stats::Scalar numConfigured;
  Stats::Scalar numStepped;
  Stats::Scalar numStreamMemRequests;
  Stats::Scalar numElements;
  Stats::Scalar numElementsUsed;
  Stats::Scalar entryWaitCycles;
  Stats::Scalar numMemElements;
  Stats::Scalar numMemElementsFetched;
  Stats::Scalar numMemElementsUsed;
  Stats::Scalar memEntryWaitCycles;

  Stats::Distribution numTotalAliveElements;
  Stats::Distribution numTotalAliveCacheBlocks;
  Stats::Distribution numRunAHeadLengthDist;
  Stats::Distribution numTotalAliveMemStreams;

private:
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