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

  bool handle(LLVMDynamicInst *inst) override;
  void tick() override;
  void regStats() override;

  bool isStreamReady(uint64_t streamId, uint64_t userSeqNum) const;
  bool canStreamStep(uint64_t streamId) const;
  void commitStreamStep(uint64_t streamId, uint64_t stepSeqNum);

  const Stream *getStreamNullable(uint64_t streamId) const;
  Stream *getStreamNullable(uint64_t streamId);
  /**
   * Stats
   */
  Stats::Scalar numConfigured;
  Stats::Scalar numStepped;

private:
  std::unordered_map<uint64_t, Stream> streamMap;

  Stream *getOrInitializeStream(
      const LLVM::TDG::TDGInstruction_StreamConfigExtra &configInst);
};

#endif