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

  /**
   * Stats
   */
  Stats::Scalar numConfigured;

private:
  std::unordered_map<uint64_t, Stream> streamMap;

  void
  initializeStreamForFirstTime(const LLVM::TDG::TDGInstruction &configInst);
};

#endif