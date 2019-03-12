#ifndef __CPU_LLVM_RENAME_STAGE__
#define __CPU_LLVM_RENAME_STAGE__

#include <vector>

#include "base/statistics.hh"
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

  LLVMRenameStage(LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu);
  LLVMRenameStage(const LLVMRenameStage &rs) = delete;
  LLVMRenameStage(LLVMRenameStage &&rs) = delete;

  void setToIEW(TimeBuffer<RenameStruct> *toIEWBuffer);
  void setFromDecode(TimeBuffer<DecodeStruct> *fromDecodeBuffer);
  void setSignal(TimeBuffer<LLVMStageSignal> *signalBuffer, int pos);

  std::string name();

  void regStats();

  void tick();
  void clearContext(ThreadID contextId) {
    this->renameStates.at(contextId).clear();
  }

  Stats::Scalar blockedCycles;
  /** Stat for total number of renamed instructions. */
  Stats::Scalar renameRenamedInsts;
  /** Stat for total number of renamed destination registers. */
  Stats::Scalar renameRenamedOperands;
  /** Stat for total number of source register rename lookups. */
  Stats::Scalar renameRenameLookups;
  Stats::Scalar intRenameLookups;
  Stats::Scalar fpRenameLookups;
  Stats::Scalar vecRenameLookups;

private:
  LLVMTraceCPU *cpu;

  unsigned renameWidth;
  unsigned maxRenameQueueSize;

  Cycles fromDecodeDelay;
  Cycles toIEWDelay;

  TimeBuffer<RenameStruct>::wire toIEW;
  TimeBuffer<DecodeStruct>::wire fromDecode;
  TimeBuffer<LLVMStageSignal>::wire signal;

  /**
   * Store all the per-context rename states.
   * Basically a per-context queue.
   */
  struct RenameState {
    std::queue<LLVMDynamicInstId> renameQueue;
    void clear() {
      assert(this->renameQueue.empty() &&
             "RenameQueue not empty when clearing context.");
    }
    RenameState() { this->clear(); }
  };

  std::vector<RenameState> renameStates;
  size_t totalRenameQueueSize;
  ThreadID lastRenamedContextId;
};

#endif