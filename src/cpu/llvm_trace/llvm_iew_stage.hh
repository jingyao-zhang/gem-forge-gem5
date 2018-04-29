#ifndef __CPU_LLVM_IEW_STAGE__
#define __CPU_LLVM_IEW_STAGE__

#include <list>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "cpu/llvm_trace/llvm_insts.hh"
#include "cpu/llvm_trace/llvm_rename_stage.hh"
#include "cpu/llvm_trace/llvm_stage_signal.hh"
#include "cpu/o3/fu_pool.hh"
#include "cpu/timebuf.hh"
#include "params/LLVMTraceCPU.hh"

class LLVMTraceCPU;
class LLVMAccelerator;

class LLVMIEWStage {
 public:
  using RenameStruct = LLVMRenameStage::RenameStruct;
  using IEWStruct = std::vector<LLVMDynamicInstId>;

  LLVMIEWStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu);
  LLVMIEWStage(const LLVMDecodeStage& other) = delete;
  LLVMIEWStage(LLVMIEWStage&& other) = delete;

  void setToCommit(TimeBuffer<IEWStruct>* toCommitBuffer);
  void setFromRename(TimeBuffer<RenameStruct>* fromRenameBuffer);
  void setSignal(TimeBuffer<LLVMStageSignal>* signalBuffer, int pos);

  void tick();

  // API for complete a function unit.
  // @param fuId: if not -1, will free this fu in next cycle.
  void processFUCompletion(LLVMDynamicInstId instId, int fuId);

  class FUCompletion : public Event {
   public:
    FUCompletion(LLVMDynamicInstId _instId, int _fuId, LLVMIEWStage* _iew,
                 bool _shouldFreeFU = false);
    // From Event.
    void process() override;
    const char* description() const override;

   private:
    LLVMDynamicInstId instId;
    int fuId;
    // Pointer back to the CPU.
    LLVMIEWStage* iew;
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

  Stats::Distribution numIssuedDist;

  Stats::Distribution numExecutingDist;

 private:
  LLVMTraceCPU* cpu;

  unsigned dispatchWidth;
  unsigned issueWidth;
  unsigned writeBackWidth;
  unsigned robSize;
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

  unsigned loadQueueN;
  unsigned storeQueueN;

  FUPool* fuPool;

  std::unordered_map<int, LLVMAccelerator*> acceleratorMap;

  void dispatch();
  void issue();
  void markReady();
  void writeback(std::list<LLVMDynamicInstId>& queue, unsigned& writebacked);
  void commit();
};

#endif