#ifndef __CPU_LLVM_COMMIT_STAGE__
#define __CPU_LLVM_COMMIT_STAGE__

#include <vector>

#include "base/statistics.hh"
#include "cpu/gem_forge/llvm_iew_stage.hh"
#include "cpu/gem_forge/llvm_insts.hh"
#include "cpu/gem_forge/llvm_stage_signal.hh"
#include "cpu/timebuf.hh"
#include "params/LLVMTraceCPU.hh"

class LLVMTraceCPU;

class LLVMCommitStage {
public:
  using IEWStruct = LLVMIEWStage::IEWStruct;

  LLVMCommitStage(LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu);
  LLVMCommitStage(const LLVMCommitStage &other) = delete;
  LLVMCommitStage(LLVMCommitStage &&other) = delete;

  void setFromIEW(TimeBuffer<IEWStruct> *fromIEWBuffer);
  void setSignal(TimeBuffer<LLVMStageSignal> *signalBuffer, int pos);

  void tick();
  void clearContext(ThreadID contextId);

  std::string name();

  void regStats();

  Stats::Vector instsCommitted;
  Stats::Vector opsCommitted;
  Stats::Vector intInstsCommitted;
  Stats::Vector fpInstsCommitted;
  Stats::Vector callInstsCommitted;
  Stats::Scalar blockedCycles;

private:
  LLVMTraceCPU *cpu;

  unsigned commitWidth;
  unsigned maxCommitQueueSize;

  Cycles fromIEWDelay;

  TimeBuffer<IEWStruct>::wire fromIEW;
  TimeBuffer<LLVMStageSignal>::wire signal;

  std::list<LLVMDynamicInstId> commitQueue;

  /**
   * Store all the per-context commit states.
   */
  struct CommitState {
    size_t commitQueueSize;
    void clear() { this->commitQueueSize = 0; }
    CommitState() { this->clear(); }
  };

  std::vector<CommitState> commitStates;
};

#endif