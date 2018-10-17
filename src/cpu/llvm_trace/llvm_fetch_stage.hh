#ifndef __CPU_LLVM_FETCH_STAGE_HH__
#define __CPU_LLVM_FETCH_STAGE_HH__

#include <vector>

#include "base/statistics.hh"
#include "cpu/llvm_trace/llvm_branch_predictor.hh"
#include "cpu/llvm_trace/llvm_insts.hh"
#include "cpu/llvm_trace/llvm_stage_signal.hh"
#include "cpu/timebuf.hh"
#include "params/LLVMTraceCPU.hh"

class LLVMTraceCPU;

class LLVMFetchStage {
public:
  using FetchStruct = std::vector<LLVMDynamicInstId>;

  LLVMFetchStage(LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu);
  LLVMFetchStage(const LLVMFetchStage &fs) = delete;
  LLVMFetchStage(LLVMFetchStage &&fs) = delete;

  ~LLVMFetchStage();

  void setToDecode(TimeBuffer<FetchStruct> *toDecodeBuffer);
  void setSignal(TimeBuffer<LLVMStageSignal> *signalBuffer, int pos);
  void regStats();
  void tick();

  std::string name();

  Stats::Scalar blockedCycles;
  Stats::Scalar branchInsts;
  Stats::Scalar branchPredMisses;
  /** Stat for total number of fetched instructions. */
  Stats::Scalar fetchedInsts;
  /** Total number of fetched branches. */
  Stats::Scalar fetchedBranches;
  /** Stat for total number of predicted branches. */
  Stats::Scalar predictedBranches;

private:
  LLVMTraceCPU *cpu;

  unsigned fetchWidth;

  /**
   * Flag to tell if we have just fetched an SerializeAfter instruction.
   * If yes, the next fetched instruction is marked SerializeBefore.
   */
  bool serializeAfter;

  Cycles toDecodeDelay;
  TimeBuffer<FetchStruct>::wire toDecode;
  TimeBuffer<LLVMStageSignal>::wire signal;

  LLVMBranchPredictor *predictor;
  uint8_t branchPreictPenalityCycles;
  LLVMDynamicInstId blockedInstId;
};

#endif