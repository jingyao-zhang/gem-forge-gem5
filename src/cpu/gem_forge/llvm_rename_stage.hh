#ifndef __CPU_LLVM_RENAME_STAGE__
#define __CPU_LLVM_RENAME_STAGE__

#include <vector>

#include "base/statistics.hh"
#include "cpu/gem_forge/llvm_decode_stage.hh"
#include "cpu/gem_forge/llvm_insts.hh"
#include "cpu/gem_forge/llvm_stage_signal.hh"
#include "cpu/timebuf.hh"
#include "params/LLVMTraceCPU.hh"

namespace gem5 {

class LLVMTraceCPU;

class LLVMRenameStage {
public:
  using DecodeStruct = LLVMDecodeStage::DecodeStruct;
  using RenameStruct = std::vector<LLVMDynamicInstId>;

  LLVMRenameStage(const LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu);
  LLVMRenameStage(const LLVMRenameStage &rs) = delete;
  LLVMRenameStage(LLVMRenameStage &&rs) = delete;

  void setToIEW(TimeBuffer<RenameStruct> *toIEWBuffer);
  void setFromDecode(TimeBuffer<DecodeStruct> *fromDecodeBuffer);
  void setSignal(TimeBuffer<LLVMStageSignal> *signalBuffer, int pos);

  std::string name();

  void regStats();

  void tick();
  void clearThread(ThreadID threadId) {
    this->renameStates.at(threadId).clear();
  }

  statistics::Scalar blockedCycles;
  /** Stat for total number of renamed instructions. */
  statistics::Scalar renameRenamedInsts;
  /** Stat for total number of renamed destination registers. */
  statistics::Scalar renameRenamedOperands;
  /** Stat for total number of source register rename lookups. */
  statistics::Scalar renameRenameLookups;
  statistics::Scalar intRenameLookups;
  statistics::Scalar fpRenameLookups;
  statistics::Scalar vecRenameLookups;

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
  ContextID lastRenamedThreadId;

  size_t getTotalRenameQueueSize() const;
};

} // namespace gem5

#endif