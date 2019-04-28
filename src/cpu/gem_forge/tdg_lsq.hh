#ifndef __CPU_TDG_LSQ_H__
#define __CPU_TDG_LSQ_H__

#include <functional>
#include <list>

class LLVMTraceCPU;
class LLVMIEWStage;

/**
 * This LSQ is designed to be abstract as the entries may not be a simple
 * instruction. For example, the stream engine may use this to handle aliasing.
 * It only knows about the interface/callback.
 */

struct GemForgeLQCallback {};

struct GemForgeSQCallback {
  std::function<void()> writeback;
  std::function<bool()> isWritebacked;
  std::function<void()> writebacked;
};

class TDGLoadStoreQueue {
public:
  TDGLoadStoreQueue(LLVMTraceCPU *_cpu, LLVMIEWStage *_iew, int _loadQueueSize,
                    int _storeQueueSize, int _cacheStorePorts);

  void insertLoad(const GemForgeLQCallback &callback);
  void insertStore(const GemForgeSQCallback &callback);

  void commitLoad();
  void commitStore();
  void postCommitStore();

  void writebackStore();

  int loads() const { return this->loadQueue.size(); }
  int stores() const { return this->storeQueue.size(); }

  const int loadQueueSize;
  const int storeQueueSize;
  const int cacheStorePorts;

private:
  LLVMTraceCPU *cpu;
  LLVMIEWStage *iew;

  struct LoadQueueEntry {
  public:
    GemForgeLQCallback callback;
    LoadQueueEntry(const GemForgeLQCallback &_callback);
  };

  struct StoreQueueEntry {
  public:
    GemForgeSQCallback callback;
    bool committed;
    bool writebacking;
    StoreQueueEntry(const GemForgeSQCallback &_callback);
  };

  std::list<LoadQueueEntry> loadQueue;
  std::list<StoreQueueEntry> storeQueue;
};

#endif