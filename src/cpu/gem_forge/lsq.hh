#ifndef __CPU_TDG_LSQ_H__
#define __CPU_TDG_LSQ_H__

#include "base/types.hh"

#include <functional>
#include <list>
#include <vector>

class LLVMTraceCPU;
class LLVMIEWStage;

/**
 * This LSQ is designed to be abstract as the entries may not be a simple
 * instruction. For example, the stream engine may use this to handle aliasing.
 * It only knows about the interface/callback.
 */

struct GemForgeLQCallback {
  /**
   * * Get the address and size of this lsq entry.
   * @return true if the address is ready.
   */
  std::function<bool(Addr &, uint32_t &)> getAddrSize;
};

struct GemForgeSQCallback {
  /**
   * * Get the address and size of this lsq entry.
   * @return true if the address is ready.
   */
  std::function<bool(Addr &, uint32_t &)> getAddrSize;
  std::function<void()> writeback;
  std::function<bool()> isWritebacked;
  std::function<void()> writebacked;
};

class GemForgeLoadStoreQueue {
 public:
  GemForgeLoadStoreQueue(LLVMTraceCPU *_cpu, LLVMIEWStage *_iew,
                         int _loadQueueSize, int _storeQueueSize,
                         int _cacheStorePorts);

  void insertLoad(const GemForgeLQCallback &callback);
  void insertStore(const GemForgeSQCallback &callback);

  void commitLoad();
  void commitStore();

  void detectAlias();
  void writebackStore();

  int loads() const { return this->loadQueue.size(); }
  int stores() const { return this->storeQueue.size(); }

  const int loadQueueSize;
  const int storeQueueSize;
  const int cacheStorePorts;

 private:
  LLVMTraceCPU *cpu;
  LLVMIEWStage *iew;

  using LSQEntryIndex = uint64_t;
  static const LSQEntryIndex INVALID_LSQ_ENTRY_INDEX = 0;
  LSQEntryIndex currentAllocatedLSQEntryIndex;

  struct LoadQueueEntry {
   public:
    const LSQEntryIndex lsqIndex;
    GemForgeLQCallback callback;
    bool isAddressReady;
    /**
     * WAR dependences. Stores the store queue entry index.
     */
    std::vector<LSQEntryIndex> WAREntryIndexes;
    LoadQueueEntry(const LSQEntryIndex _lsqIndex,
                   const GemForgeLQCallback &_callback);
  };

  struct StoreQueueEntry {
   public:
    const LSQEntryIndex lsqIndex;
    GemForgeSQCallback callback;
    bool isAddressReady;
    bool committed;
    bool writebacking;
    /**
     * XAW dependences. Stores either load queue entry index or store queue
     * entry index.
     */
    std::vector<LSQEntryIndex> XAWEntryIndexes;
    StoreQueueEntry(const LSQEntryIndex _lsqIndex,
                    const GemForgeSQCallback &_callback);
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