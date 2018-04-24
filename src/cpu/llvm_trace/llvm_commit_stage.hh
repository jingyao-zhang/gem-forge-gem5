#ifndef __CPU_LLVM_COMMIT_STAGE__
#define __CPU_LLVM_COMMIT_STAGE__

#include <vector>

#include "base/statistics.hh"
#include "cpu/llvm_trace/llvm_iew_stage.hh"
#include "cpu/llvm_trace/llvm_insts.hh"
#include "cpu/llvm_trace/llvm_stage_signal.hh"
#include "cpu/timebuf.hh"
#include "params/LLVMTraceCPU.hh"

class LLVMTraceCPU;

class LLVMCommitStage {
 public:
  using IEWStruct = LLVMIEWStage::IEWStruct;

  LLVMCommitStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu);
  LLVMCommitStage(const LLVMCommitStage& other) = delete;
  LLVMCommitStage(LLVMCommitStage&& other) = delete;

  void setFromIEW(TimeBuffer<IEWStruct>* fromIEWBuffer);
  void setSignal(TimeBuffer<LLVMStageSignal>* signalBuffer, int pos);

  void tick();

  void regStats();

  Stats::Vector instsCommitted;
  Stats::Vector opsCommitted;

 private:
  LLVMTraceCPU* cpu;

  Cycles fromIEWDelay;

  TimeBuffer<IEWStruct>::wire fromIEW;
  TimeBuffer<LLVMStageSignal>::wire signal;
};

#endif