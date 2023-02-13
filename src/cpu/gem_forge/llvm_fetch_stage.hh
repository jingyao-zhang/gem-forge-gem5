#ifndef __CPU_LLVM_FETCH_STAGE_HH__
#define __CPU_LLVM_FETCH_STAGE_HH__

#include <vector>

#include "base/statistics.hh"
#include "cpu/gem_forge/llvm_branch_predictor.hh"
#include "cpu/gem_forge/llvm_insts.hh"
#include "cpu/gem_forge/llvm_stage_signal.hh"
#include "cpu/pred/bpred_unit.hh"
#include "cpu/timebuf.hh"
#include "params/LLVMTraceCPU.hh"

namespace gem5 {

class LLVMTraceCPU;
class LLVMTraceThreadContext;

class LLVMFetchStage {
public:
  using FetchStruct = std::vector<LLVMDynamicInstId>;

  LLVMFetchStage(const LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu);
  LLVMFetchStage(const LLVMFetchStage &fs) = delete;
  LLVMFetchStage(LLVMFetchStage &&fs) = delete;

  ~LLVMFetchStage();

  void setToDecode(TimeBuffer<FetchStruct> *toDecodeBuffer);
  void setSignal(TimeBuffer<LLVMStageSignal> *signalBuffer, int pos);
  void regStats();
  void tick();
  void clearThread(ThreadID threadId);

  std::string name();

  statistics::Scalar blockedCycles;
  statistics::Scalar branchInsts;
  statistics::Scalar branchPredMisses;
  /** Stat for total number of fetched instructions. */
  statistics::Scalar fetchedInsts;
  /** Total number of fetched branches. */
  statistics::Scalar fetchedBranches;
  /** Stat for total number of predicted branches. */
  statistics::Scalar predictedBranches;
  /** Total number of our original 2-bit prediction miss. */
  statistics::Scalar branchPredMissesLLVM;
  /** Total number of gem5 prediction miss. */
  statistics::Scalar branchPredMissesGem5;

private:
  LLVMTraceCPU *cpu;

  unsigned fetchWidth;

  /**
   * Stores all the per-context fetch states.
   */
  struct FetchState {
    /**
     * Flag to tell if we have just fetched an SerializeAfter instruction.
     * If yes, the next fetched instruction is marked SerializeBefore.
     */
    bool serializeAfter;
    // Penality cycles after the branch instruction is committed.
    uint8_t branchPredictPenalityCycles;
    // Branch missed instruction id.
    LLVMDynamicInstId blockedInstId;
    // TODO: Make the predictor also context-base.
    FetchState() : serializeAfter(false), branchPredictPenalityCycles(0) {}

    void clear() {
      this->serializeAfter = false;
      this->branchPredictPenalityCycles = 0;
    }
  };

  std::vector<FetchState> fetchStates;

  Cycles toDecodeDelay;
  bool useGem5BranchPredictor;
  TimeBuffer<FetchStruct>::wire toDecode;
  TimeBuffer<LLVMStageSignal>::wire signal;

  LLVMBranchPredictor *predictor;

  branch_prediction::BPredUnit *branchPredictor;

  int lastFetchedThreadId;
};

} // namespace gem5

#endif
