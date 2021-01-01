#ifndef __CPU_TDG_LSQ_H__
#define __CPU_TDG_LSQ_H__

#include "gem_forge_lsq_callback.hh"

#include "base/statistics.hh"

#include <functional>
#include <list>
#include <memory>
#include <vector>

class LLVMTraceCPU;
class LLVMIEWStage;

class GemForgeLoadStoreQueue {
public:
  GemForgeLoadStoreQueue(LLVMTraceCPU *_cpu, LLVMIEWStage *_iew,
                         int _loadQueueSize, int _storeQueueSize,
                         int _cacheStorePorts);

  void insertLoad(std::unique_ptr<GemForgeLQCallback> callback);
  void insertStore(std::unique_ptr<GemForgeSQDeprecatedCallback> callback);

  void commitLoad();
  void commitStore();

  void detectAlias();
  void writebackStore();

  int loads() const { return this->loadQueue.size(); }
  int stores() const { return this->storeQueue.size(); }

  void regStats();

  const int loadQueueSize;
  const int storeQueueSize;
  const int cacheStorePorts;

  mutable Stats::Scalar LQEntriesAllocated;
  mutable Stats::Scalar SQEntriesAllocated;

private:
  LLVMTraceCPU *cpu;
  LLVMIEWStage *iew;

  using LSQEntryIndex = uint64_t;
  static const LSQEntryIndex INVALID_LSQ_ENTRY_INDEX = 0;
  LSQEntryIndex currentAllocatedLSQEntryIndex;

  struct LoadQueueEntry {
  public:
    const LSQEntryIndex lsqIndex;
    std::unique_ptr<GemForgeLQCallback> callback;
    bool isAddressReady;
    /**
     * WAR dependences. Stores the store queue entry index.
     */
    std::vector<LSQEntryIndex> WAREntryIndexes;
    LoadQueueEntry(const LSQEntryIndex _lsqIndex,
                   std::unique_ptr<GemForgeLQCallback> _callback);
  };

  struct StoreQueueEntry {
  public:
    const LSQEntryIndex lsqIndex;
    std::unique_ptr<GemForgeSQDeprecatedCallback> callback;
    bool isAddressReady;
    bool committed;
    bool writebacking;
    /**
     * XAW dependences. Stores either load queue entry index or store queue
     * entry index.
     */
    std::vector<LSQEntryIndex> XAWEntryIndexes;
    StoreQueueEntry(const LSQEntryIndex _lsqIndex,
                    std::unique_ptr<GemForgeSQDeprecatedCallback> _callback);
  };

  std::list<LoadQueueEntry> loadQueue;
  std::list<StoreQueueEntry> storeQueue;

  void checkLoadStoreAlias(LoadQueueEntry &loadEntry,
                           StoreQueueEntry &storeEntry);
  void checkStoreStoreAlias(StoreQueueEntry &storeEntry1,
                            StoreQueueEntry &storeEntry2);
  static bool isAliased(Addr addr1, uint32_t size1, Addr addr2, uint32_t size2);
};

#endif