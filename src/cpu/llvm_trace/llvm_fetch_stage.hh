#ifndef __CPU_LLVM_FETCH_STAGE_HH__
#define __CPU_LLVM_FETCH_STAGE_HH__

#include <vector>

#include "base/statistics.hh"
#include "cpu/llvm_trace/llvm_insts.hh"
#include "cpu/llvm_trace/llvm_stage_signal.hh"
#include "cpu/timebuf.hh"
#include "params/LLVMTraceCPU.hh"

class LLVMTraceCPU;

class LLVMFetchStage {
 public:
  using FetchStruct = std::vector<LLVMDynamicInstId>;

  LLVMFetchStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu);
  LLVMFetchStage(const LLVMFetchStage& fs) = delete;
  LLVMFetchStage(LLVMFetchStage&& fs) = delete;

  void setToDecode(TimeBuffer<FetchStruct>* toDecodeBuffer);
  void setSignal(TimeBuffer<LLVMStageSignal>* signalBuffer, int pos);
  void regStats();
  void tick();

  Stats::Scalar blockedCycles;

 private:
  LLVMTraceCPU* cpu;

  unsigned fetchWidth;

  Cycles toDecodeDelay;
  TimeBuffer<FetchStruct>::wire toDecode;
  TimeBuffer<LLVMStageSignal>::wire signal;
};

#endif