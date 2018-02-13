#ifndef __CPU_LLVM_DECODE_STAGE
#define __CPU_LLVM_DECODE_STAGE

#include <queue>
#include <vector>

#include "cpu/llvm_trace/llvm_fetch_stage.hh"
#include "cpu/llvm_trace/llvm_insts.hh"
#include "cpu/llvm_trace/llvm_stage_signal.hh"
#include "cpu/timebuf.hh"
#include "params/LLVMTraceCPU.hh"

class LLVMTraceCPU;

class LLVMDecodeStage {
 public:
  using FetchStruct = LLVMFetchStage::FetchStruct;
  using DecodeStruct = std::vector<LLVMDynamicInstId>;

  LLVMDecodeStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu);
  LLVMDecodeStage(const LLVMDecodeStage& ds) = delete;
  LLVMDecodeStage(LLVMDecodeStage&& ds) = delete;

  void setToRename(TimeBuffer<DecodeStruct>* toRenameBuffer);
  void setFromFetch(TimeBuffer<FetchStruct>* fromFetchBuffer);
  void setSignal(TimeBuffer<LLVMStageSignal>* signalBuffer, int pos);

  void regStats();

  void tick();

 private:
  LLVMTraceCPU* cpu;

  unsigned decodeWidth;
  unsigned decodeQueueSize;

  Cycles fromFetchDelay;
  Cycles toRenameDelay;

  TimeBuffer<DecodeStruct>::wire toRename;
  TimeBuffer<FetchStruct>::wire fromFetch;
  TimeBuffer<LLVMStageSignal>::wire signal;

  std::queue<LLVMDynamicInstId> decodeQueue;
};

#endif
