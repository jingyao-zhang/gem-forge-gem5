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
  void clearThread(ThreadID threadId) { this->iewStates.at(threadId).clear(); }

  GemForgeLoadStoreQueue *getLSQ() { return this->lsq; }
  LLVMDynamicInstId getROBHeadInstId() const {
    if (!this->rob.empty()) {
      return this->rob.front();
    } else {
      return LLVMDynamicInst::INVALID_INST_ID;
    }
  }

  void dumpROB() const;

  /**
   * API for other stages and units.
   */
  void commitInst(LLVMDynamicInstId instId);
  void postCommitInst(LLVMDynamicInstId instId);
  void blockMemInst(LLVMDynamicInstId instId);
  void unblockMemoryInsts();

  /**
   * Add misspeculation penalty to a thread.
   */
  void misspeculateInst(LLVMDynamicInst *inst);

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
  Stats::Scalar MisSpecRAWDependence;

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
    int misspecPenaltyCycles;
    void clear() {
      assert(this->robSize == 0 && "ROB not empty when clear context.");
    }
    IEWState() : robSize(0), misspecPenaltyCycles(0) { this->clear(); }
  };

  std::vector<IEWState> iewStates;

  void dispatch();
  void issue();
  void markReady();
  void writeback(std::list<LLVMDynamicInstId> &queue, unsigned &writebacked);
  void sendToCommit();

  /**
   * * Callback structures for the lsq.
   * * Since C++11, the nested class has the same access right as the
   * * enclosing class. We make these callbacks nested to have the same
   * * access right to LLVMTraceCPU as the IEWStage.
   */
  struct GemForgeIEWLQCallback : public GemForgeLQCallback {
  public:
    LLVMDynamicInst *inst;
    LLVMTraceCPU *cpu;
    GemForgeIEWLQCallback(LLVMDynamicInst *_inst, LLVMTraceCPU *_cpu)
        : inst(_inst), cpu(_cpu) {}
    bool getAddrSize(Addr &addr, uint32_t &size) const override {
      assert(inst->getTDG().has_load() && "Missing loadExtra for load inst.");
      addr = inst->getTDG().load().addr();
      size = inst->getTDG().load().size();
      return true;
    }
    bool hasNonCoreDependent() const override { return false; }
    bool isIssued() const override;
    bool isValueReady() const override;
    void RAWMisspeculate() override;
  };
  struct GemForgeIEWSQCallback : public GemForgeSQDeprecatedCallback {
  public:
    LLVMDynamicInst *inst;
    LLVMTraceCPU *cpu;
    GemForgeIEWSQCallback(LLVMDynamicInst *_inst, LLVMTraceCPU *_cpu)
        : inst(_inst), cpu(_cpu) {}
    bool getAddrSize(Addr &addr, uint32_t &size) override {
      assert(inst->getTDG().has_store() &&
             "Missing storeExtra for store inst.");
      addr = inst->getTDG().store().addr();
      size = inst->getTDG().store().size();
      return true;
    }
    void writeback() override { inst->writeback(cpu); }
    bool isWritebacked() override { return inst->isWritebacked(); }
    void writebacked() override;
  };
};

#endif
