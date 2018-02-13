#ifndef __CPU_LLVM_RENAME_STAGE__
#define __CPU_LLVM_RENAME_STAGE__

#include <vector>

#include "cpu/llvm_trace/llvm_decode_stage.hh"
#include "cpu/llvm_trace/llvm_insts.hh"
#include "cpu/llvm_trace/llvm_stage_signal.hh"
#include "cpu/timebuf.hh"
#include "params/LLVMTraceCPU.hh"

class LLVMTraceCPU;

class LLVMRenameStage {
 public:
  using DecodeStruct = LLVMDecodeStage::DecodeStruct;
  using RenameStruct = std::vector<LLVMDynamicInstId>;

  LLVMRenameStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu);
  LLVMRenameStage(const LLVMRenameStage& rs) = delete;
  LLVMRenameStage(LLVMRenameStage&& rs) = delete;

  void setToIEW(TimeBuffer<RenameStruct>* toIEWBuffer);
  void setFromDecode(TimeBuffer<DecodeStruct>* fromDecodeBuffer);
  void setSignal(TimeBuffer<LLVMStageSignal>* signalBuffer, int pos);

  void regStats();

  void tick();

 private:
  LLVMTraceCPU* cpu;

  unsigned renameWidth;
  unsigned robSize;

  Cycles fromDecodeDelay;
  Cycles toIEWDelay;

  TimeBuffer<RenameStruct>::wire toIEW;
  TimeBuffer<DecodeStruct>::wire fromDecode;
  TimeBuffer<LLVMStageSignal>::wire signal;

  std::list<LLVMDynamicInstId> rob;
};

#endif