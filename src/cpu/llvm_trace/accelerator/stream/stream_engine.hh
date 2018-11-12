#ifndef __CPU_TDG_ACCELERATOR_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_STREAM_ENGINE_H__

#include "insts.hh"
#include "stream.hh"

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
  void commitStreamConfigure(uint64_t streamId, uint64_t configSeqNum);
  void commitStreamStep(uint64_t streamId, uint64_t stepSeqNum);
  void commitStreamStore(uint64_t streamId, uint64_t storeSeqNum);
  void commitStreamEnd(uint64_t streamId, uint64_t endSeqNum);

  const Stream *getStreamNullable(uint64_t streamId) const;
  Stream *getStreamNullable(uint64_t streamId);
  /**
   * Stats
   */
  Stats::Scalar numConfigured;
  Stats::Scalar numStepped;
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
  std::unordered_map<uint64_t, Stream> streamMap;

  bool isOracle;
  unsigned maxRunAHeadLength;
  std::string throttling;

  Stream *getOrInitializeStream(
      const LLVM::TDG::TDGInstruction_StreamConfigExtra &configInst);

  void updateAliveStatistics();
};

#endif