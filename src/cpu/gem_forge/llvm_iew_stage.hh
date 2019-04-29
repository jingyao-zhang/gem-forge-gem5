#ifndef __CPU_LLVM_IEW_STAGE__
#define __CPU_LLVM_IEW_STAGE__

#include <list>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "cpu/gem_forge/llvm_insts.hh"
#include "cpu/gem_forge/llvm_rename_stage.hh"
#include "cpu/gem_forge/llvm_stage_signal.hh"
#include "cpu/timebuf.hh"
#include "lsq.hh"
#include "params/LLVMTraceCPU.hh"

class LLVMTraceCPU;

class LLVMIEWStage {
 public:
  using RenameStruct = LLVMRenameStage::RenameStruct;
  using IEWStruct = std::vector<LLVMDynamicInstId>;

  LLVMIEWStage(LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu);
  LLVMIEWStage(const LLVMDecodeStage &other) = delete;
  LLVMIEWStage(LLVMIEWStage &&other) = delete;

  void setToCommit(TimeBuffer<IEWStruct> *toCommitBuffer);
  void setFromRename(TimeBuffer<RenameStruct> *fromRenameBuffer);
  void setSignal(TimeBuffer<LLVMStageSignal> *signalBuffer, int pos);

  void tick();
  void clearContext(ThreadID contextId) {
    this->iewStates.at(contextId).clear();
  }

  GemForgeLoadStoreQueue *getLSQ() { return this->lsq; }

  void dumpROB() const;

  /**
   * API for other stages and units.
   */
  void commitInst(LLVMDynamicInstId instId);
  void postCommitInst(LLVMDynamicInstId instId);
  void blockMemInst(LLVMDynamicInstId instId);
  void unblockMemoryInsts();

  std::string name();

  // API for complete a function unit.
  // @param fuId: if not -1, will free this fu in next cycle.
  void processFUCompletion(LLVMDynamicInstId instId, int fuId);

  class FUCompletion : public Event {
   public:
    FUCompletion(LLVMDynamicInstId _instId, int _fuId, LLVMIEWStage *_iew,
                 bool _shouldFreeFU = false);
    // From Event.
    void process() override;
    const char *description() const override;

   private:
    LLVMDynamicInstId instId;
    int fuId;
    // Pointer back to the CPU.
    LLVMIEWStage *iew;
    // Should the FU be freed next cycle.
    // Used for un-pipelined FU.
    // Default is false.
    bool shouldFreeFU;
  };

  /*******************************************************************/
  // All the statics.
  /*******************************************************************/
  void regStats();
  // Stat for total number issued for each instruction type.
  Stats::Vector2d statIssuedInstType;

  Stats::Scalar blockedCycles;

  Stats::Scalar robReads;
  Stats::Scalar robWrites;
  Stats::Scalar intInstQueueReads;
  Stats::Scalar intInstQueueWrites;
  Stats::Scalar intInstQueueWakeups;
  Stats::Scalar fpInstQueueReads;
  Stats::Scalar fpInstQueueWrites;
  Stats::Scalar fpInstQueueWakeups;
  Stats::Scalar loadQueueWrites;
  Stats::Scalar storeQueueWrites;
  Stats::Scalar RAWDependenceInLSQ;
  Stats::Scalar WAWDependenceInLSQ;
  Stats::Scalar WARDependenceInLSQ;

  Stats::Scalar intRegReads;
  Stats::Scalar intRegWrites;
  Stats::Scalar fpRegReads;
  Stats::Scalar fpRegWrites;

  /**
   * ALU for integer +/-.
   * Mult for integer multiply and division.
   * FPU for all float operation.
   */
  Stats::Scalar ALUAccesses;
  Stats::Scalar MultAccesses;
  Stats::Scalar FPUAccesses;

  /**
   * The difference between these stats and *Accesses is
   * that multi-cycle operation will be counted multiple times,
   * which is required by mcpat to compute the power.
   */
  Stats::Scalar ALUAccessesCycles;
  Stats::Scalar MultAccessesCycles;
  Stats::Scalar FPUAccessesCycles;

  /**
   * Load/Store statistics.
   */
  Stats::Scalar execLoadInsts;
  Stats::Scalar execStoreInsts;

  Stats::Distribution numIssuedDist;

  Stats::Distribution numExecutingDist;

 private:
  LLVMTraceCPU *cpu;

  unsigned dispatchWidth;
  unsigned issueWidth;
  unsigned writeBackWidth;
  unsigned maxRobSize;
  unsigned cacheLoadPorts;
  unsigned instQueueSize;
  unsigned loadQueueSize;
  unsigned storeQueueSize;

  Cycles fromRenameDelay;
  Cycles toCommitDelay;

  TimeBuffer<IEWStruct>::wire toCommit;
  TimeBuffer<RenameStruct>::wire fromRename;
  TimeBuffer<LLVMStageSignal>::wire signal;

  std::list<LLVMDynamicInstId> rob;
  std::list<LLVMDynamicInstId> instQueue;

  GemForgeLoadStoreQueue *lsq;

  struct IEWState {
    size_t robSize;
    void clear() {
      assert(this->robSize == 0 && "ROB not empty when clear context.");
    }
    IEWState() : robSize(0) { this->clear(); }
  };

  std::vector<IEWState> iewStates;

  void dispatch();
  void issue();
  void markReady();
  void writeback(std::list<LLVMDynamicInstId> &queue, unsigned &writebacked);
  void sendToCommit();
};

#endif