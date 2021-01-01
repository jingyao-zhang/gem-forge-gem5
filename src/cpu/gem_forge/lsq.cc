#include "lsq.hh"

#include "llvm_iew_stage.hh"
#include "llvm_trace_cpu.hh"

#include "debug/GemForgeLoadStoreQueue.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

GemForgeLoadStoreQueue::LoadQueueEntry::LoadQueueEntry(
    const LSQEntryIndex _lsqIndex,
    std::unique_ptr<GemForgeLQCallback> _callback)
    : lsqIndex(_lsqIndex), callback(std::move(_callback)),
      isAddressReady(false) {}

GemForgeLoadStoreQueue::StoreQueueEntry::StoreQueueEntry(
    const LSQEntryIndex _lsqIndex,
    std::unique_ptr<GemForgeSQDeprecatedCallback> _callback)
    : lsqIndex(_lsqIndex), callback(std::move(_callback)),
      isAddressReady(false), committed(false), writebacking(false) {}

GemForgeLoadStoreQueue::GemForgeLoadStoreQueue(LLVMTraceCPU *_cpu,
                                               LLVMIEWStage *_iew,
                                               int _loadQueueSize,
                                               int _storeQueueSize,
                                               int _cacheStorePorts)
    : loadQueueSize(_loadQueueSize), storeQueueSize(_storeQueueSize),
      cacheStorePorts(_cacheStorePorts), cpu(_cpu), iew(_iew),
      currentAllocatedLSQEntryIndex(INVALID_LSQ_ENTRY_INDEX) {}

void GemForgeLoadStoreQueue::regStats() {
#define scalar(stat, describe)                                                 \
  this->stat.name(this->iew->name() + (".lsq." #stat))                         \
      .desc(describe)                                                          \
      .prereq(this->stat)
  scalar(LQEntriesAllocated, "LQ entries allocated.");
  scalar(SQEntriesAllocated, "SQ entries allocated.");
}

void GemForgeLoadStoreQueue::insertLoad(
    std::unique_ptr<GemForgeLQCallback> callback) {
  panic_if(this->loadQueue.size() >= this->loadQueueSize,
           "Load queue overflows.");
  auto lsqIndex = ++this->currentAllocatedLSQEntryIndex;
  this->loadQueue.emplace_back(lsqIndex, std::move(callback));
  this->iew->loadQueueWrites++;
  this->LQEntriesAllocated++;
}

void GemForgeLoadStoreQueue::insertStore(
    std::unique_ptr<GemForgeSQDeprecatedCallback> callback) {
  if (this->storeQueue.size() >= this->storeQueueSize) {
    panic("Store queue overflows.");
  }
  auto lsqIndex = ++this->currentAllocatedLSQEntryIndex;
  this->storeQueue.emplace_back(lsqIndex, std::move(callback));
  this->iew->storeQueueWrites++;
  this->SQEntriesAllocated++;
}

void GemForgeLoadStoreQueue::commitLoad() {
  if (this->loadQueue.empty()) {
    panic("Failed to find the commit load instruction.");
  }
  this->loadQueue.pop_front();
}

void GemForgeLoadStoreQueue::commitStore() {
  auto commitIter = this->storeQueue.begin();
  while (commitIter != this->storeQueue.end() && commitIter->committed) {
    commitIter++;
  }
  if (commitIter == this->storeQueue.end()) {
    panic("Try to commit store when there is no instruction in the store "
          "queue waiting to be committed.");
  }
  commitIter->committed = true;
}

void GemForgeLoadStoreQueue::writebackStore() {
  auto writebackIter = this->storeQueue.begin();
  auto usedStorePorts = 0;
  while (writebackIter != this->storeQueue.end() && writebackIter->committed) {
    if (!writebackIter->writebacking) {
      // We start to writeback it.
      writebackIter->writebacking = true;
      writebackIter->callback->writeback();
      usedStorePorts++;
      if (usedStorePorts >= this->cacheStorePorts) {
        // We are done for this cycle.
        break;
      }

    } else {
      // We should check if it is writebacked.
      // if (inst->isWritebacked()) {
      if (writebackIter->callback->isWritebacked()) {
        // If so, update it to writebacked and remove from the store queue.
        // Continue to try to write back the next store in queue.
        writebackIter->callback->writebacked();
        writebackIter = this->storeQueue.erase(writebackIter);
        continue;
      } else {
        // The head of the store queue is not done.
        break;
      }
    }
  }
}

void GemForgeLoadStoreQueue::detectAlias() {
  // Try to detect the aliasing for WAR.
  for (auto &loadEntry : this->loadQueue) {
    if (loadEntry.isAddressReady) {
      // We have already processed this entry.
      continue;
    }
    Addr loadAddr;
    uint32_t loadSize;
    if (!loadEntry.callback->getAddrSize(loadAddr, loadSize)) {
      // This load queue still does not know the address.
      continue;
    }
    // This load queue entry just get the address.
    loadEntry.isAddressReady = true;
    // Search through the store queue backwards.
    for (auto storeQueueIter = this->storeQueue.rbegin(),
              storeQueueEnd = this->storeQueue.rend();
         storeQueueIter != storeQueueEnd; ++storeQueueIter) {
      auto &storeEntry = *storeQueueIter;
      if (storeEntry.committed) {
        // Ignore entry in the store buffer.
        continue;
      }
      if (!storeEntry.isAddressReady) {
        // This is not ready yet.
        continue;
      }
      this->checkLoadStoreAlias(loadEntry, storeEntry);
    }
  }
  for (auto &storeEntry : this->storeQueue) {
    if (storeEntry.committed) {
      // Ignore entry in the store buffer.
      continue;
    }
    if (storeEntry.isAddressReady) {
      // We have already processed this entry.
      continue;
    }
    Addr storeAddr;
    uint32_t storeSize;
    if (!storeEntry.callback->getAddrSize(storeAddr, storeSize)) {
      // This entry still does not know the address.
      continue;
    }
    // This store queue entry just get the address.
    storeEntry.isAddressReady = true;
    // Search through the load queue.
    for (auto &loadEntry : this->loadQueue) {
      if (!loadEntry.isAddressReady) {
        continue;
      }
      this->checkLoadStoreAlias(loadEntry, storeEntry);
    }
    // Search through the store queue.
    for (auto &storeEntry2 : this->storeQueue) {
      if (storeEntry2.committed) {
        // Ignore entry in the store buffer.
        continue;
      }
      if (!storeEntry2.isAddressReady) {
        continue;
      }
      if (storeEntry2.lsqIndex == storeEntry.lsqIndex) {
        continue;
      }
      this->checkStoreStoreAlias(storeEntry, storeEntry2);
    }
  }
}

void GemForgeLoadStoreQueue::checkLoadStoreAlias(LoadQueueEntry &loadEntry,
                                                 StoreQueueEntry &storeEntry) {
  Addr loadAddr;
  uint32_t loadSize;
  assert(loadEntry.isAddressReady && "LoadEntry should be address ready.");
  assert(loadEntry.callback->getAddrSize(loadAddr, loadSize) &&
         "Failed to get the address for load entry.");
  Addr storeAddr;
  uint32_t storeSize;
  assert(storeEntry.callback->getAddrSize(storeAddr, storeSize) &&
         "Failed to get the address for store entry.");
  auto aliased = GemForgeLoadStoreQueue::isAliased(loadAddr, loadSize,
                                                   storeAddr, storeSize);
  if (!aliased) {
    // Not aliased.
    return;
  }
  if (loadEntry.lsqIndex < storeEntry.lsqIndex) {
    // WAR dependence.
    this->iew->WARDependenceInLSQ++;
    loadEntry.WAREntryIndexes.push_back(storeEntry.lsqIndex);
  } else {
    // RAW dependence.
    this->iew->RAWDependenceInLSQ++;
    storeEntry.XAWEntryIndexes.push_back(loadEntry.lsqIndex);
    if (loadEntry.callback->isIssued()) {
      // This load entry has been issued.
      // There is an mis-speculation.
      this->iew->MisSpecRAWDependence++;
      // Inform the user that there is an RAW misspeculation.
      loadEntry.callback->RAWMisspeculate();
      // // Revert the load queue entry to not knowing the address.
      // loadEntry.isAddressReady = false;
      // loadEntry.WAREntryIndexes.clear();
    }
  }
}

void GemForgeLoadStoreQueue::checkStoreStoreAlias(
    StoreQueueEntry &storeEntry1, StoreQueueEntry &storeEntry2) {
  assert(storeEntry1.lsqIndex != storeEntry2.lsqIndex &&
         "Cannot check alias for the same store queue entry.");
  Addr storeAddr1, storeAddr2;
  uint32_t storeSize1, storeSize2;
  assert(storeEntry1.callback->getAddrSize(storeAddr1, storeSize1) &&
         "Failed to get the address for first store entry.");
  assert(storeEntry2.callback->getAddrSize(storeAddr2, storeSize2) &&
         "Failed to get the address for second store entry.");
  auto aliased = GemForgeLoadStoreQueue::isAliased(storeAddr1, storeSize1,
                                                   storeAddr2, storeSize2);
  if (!aliased) {
    // Not aliased.
    return;
  }
  // WAW dependence.
  this->iew->WAWDependenceInLSQ++;
  if (storeEntry1.lsqIndex < storeEntry2.lsqIndex) {
    storeEntry1.XAWEntryIndexes.push_back(storeEntry2.lsqIndex);
  } else {
    storeEntry2.XAWEntryIndexes.push_back(storeEntry1.lsqIndex);
  }
}

bool GemForgeLoadStoreQueue::isAliased(Addr addr1, uint32_t size1, Addr addr2,
                                       uint32_t size2) {
  return !((addr1 + size1 <= addr2) || (addr2 + size2 <= addr1));
}