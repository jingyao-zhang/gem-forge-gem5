#ifndef __CPU_LLVM_DECODE_STAGE
#define __CPU_LLVM_DECODE_STAGE

#include <queue>
#include <vector>

#include "base/statistics.hh"
#include "cpu/gem_forge/llvm_fetch_stage.hh"
#include "cpu/gem_forge/llvm_insts.hh"
#include "cpu/gem_forge/llvm_stage_signal.hh"
#include "cpu/timebuf.hh"
#include "params/LLVMTraceCPU.hh"

namespace gem5 {

class LLVMTraceCPU;

class LLVMDecodeStage {
public:
  using FetchStruct = LLVMFetchStage::FetchStruct;
  using DecodeStruct = std::vector<LLVMDynamicInstId>;

  LLVMDecodeStage(const LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu);
  LLVMDecodeStage(const LLVMDecodeStage &ds) = delete;
  LLVMDecodeStage(LLVMDecodeStage &&ds) = delete;

  std::string name();

  void setToRename(TimeBuffer<DecodeStruct> *toRenameBuffer);
  void setFromFetch(TimeBuffer<FetchStruct> *fromFetchBuffer);
  void setSignal(TimeBuffer<LLVMStageSignal> *signalBuffer, int pos);

  void regStats();

  void tick();
  void clearThread(ThreadID threadId) {
    this->decodeStates.at(threadId).clear();
  }

  statistics::Scalar blockedCycles;

  /** Stat for total number of decoded instructions. */
  statistics::Scalar decodeDecodedInsts;

private:
  LLVMTraceCPU *cpu;

  unsigned decodeWidth;
  unsigned maxDecodeQueueSize;

  Cycles fromFetchDelay;
  Cycles toRenameDelay;

  TimeBuffer<DecodeStruct>::wire toRename;
  TimeBuffer<FetchStruct>::wire fromFetch;
  TimeBuffer<LLVMStageSignal>::wire signal;

  /**
   * Store all the per-context decode states.
   * Basically a per-context queue.
   */
  struct DecodeState {
    std::queue<LLVMDynamicInstId> decodeQueue;
    void clear() {
      assert(this->decodeQueue.empty() &&
             "DecodeQueue not empty when clearing context.");
    }
    DecodeState() { this->clear(); }
  };

  std::vector<DecodeState> decodeStates;
  ContextID lastDecodedThreadId;

  /**
   * Compute the total decode queue size.
   * ! This will ignore the ideal thread.
   */
  size_t getTotalDecodeQueueSize() const;
};

} // namespace gem5

#endif
