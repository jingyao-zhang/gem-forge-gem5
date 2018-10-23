#ifndef __CPU_TDG_LSQ_H__
#define __CPU_TDG_LSQ_H__

#include "llvm_insts.hh"

#include <list>

class LLVMTraceCPU;
class LLVMIEWStage;
class TDGLoadStoreQueue {
public:
  TDGLoadStoreQueue(LLVMTraceCPU *_cpu, LLVMIEWStage *_iew, int _loadQueueSize,
                    int _storeQueueSize);

  void insertLoad(LLVMDynamicInstId instId);
  void insertStore(LLVMDynamicInstId instId);

  void executeLoad(LLVMDynamicInstId instId);
  void executeStore(LLVMDynamicInstId instId);

  void commitLoad(LLVMDynamicInstId instId);
  void commitStore(LLVMDynamicInstId instId);
  void postCommitLoad(LLVMDynamicInstId instId);
  void postCommitStore(LLVMDynamicInstId instId);

  void writebackStore();

  size_t loads() const { return this->loadQueue.size(); }
  size_t stores() const { return this->storeQueue.size(); }

private:

  LLVMTraceCPU *cpu;
  LLVMIEWStage *iew;

  const int loadQueueSize;
  const int storeQueueSize;

  struct StoreQueueEntry {
  public:
    LLVMDynamicInstId id;
    bool committed;
    bool writebacking;
    StoreQueueEntry(LLVMDynamicInstId _id);
  };

  std::list<LLVMDynamicInstId> loadQueue;
  std::list<StoreQueueEntry> storeQueue;
};

#endif