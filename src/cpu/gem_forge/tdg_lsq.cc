#include "tdg_lsq.hh"

#include "llvm_iew_stage.hh"
#include "llvm_trace_cpu.hh"

#include "debug/TDGLoadStoreQueue.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

TDGLoadStoreQueue::TDGLoadStoreQueue(LLVMTraceCPU *_cpu, LLVMIEWStage *_iew,
                                     int _loadQueueSize, int _storeQueueSize,
                                     int _cacheStorePorts)
    : loadQueueSize(_loadQueueSize), storeQueueSize(_storeQueueSize),
      cacheStorePorts(_cacheStorePorts), cpu(_cpu), iew(_iew) {}

void TDGLoadStoreQueue::insertLoad(const GemForgeLQCallback &callback) {
  panic_if(this->loadQueue.size() >= this->loadQueueSize,
           "Load queue overflows.");
  this->loadQueue.emplace_back(callback);
}

void TDGLoadStoreQueue::insertStore(const GemForgeSQCallback &callback) {
  if (this->storeQueue.size() >= this->storeQueueSize) {
    panic("Store queue overflows.");
  }
  this->storeQueue.emplace_back(callback);
}

void TDGLoadStoreQueue::postCommitLoad() {
  if (this->loadQueue.empty()) {
    panic("Failed to find the commit load instruction.");
  }
  this->loadQueue.pop_front();
}

void TDGLoadStoreQueue::postCommitStore() {}

void TDGLoadStoreQueue::commitLoad() {}

void TDGLoadStoreQueue::commitStore() {
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

void TDGLoadStoreQueue::writebackStore() {
  auto writebackIter = this->storeQueue.begin();
  auto usedStorePorts = 0;
  while (writebackIter != this->storeQueue.end() && writebackIter->committed) {
    if (!writebackIter->writebacking) {
      // We start to writeback it.
      writebackIter->writebacking = true;
      writebackIter->callback.writeback();
      usedStorePorts++;
      if (usedStorePorts >= this->cacheStorePorts) {
        // We are done for this cycle.
        break;
      }

    } else {

      // We should check if it is writebacked.
      // if (inst->isWritebacked()) {
      if (writebackIter->callback.isWritebacked()) {
        // If so, update it to writebacked and remove from the store queue.
        // Continue to try to write back the next store in queue.
        writebackIter->callback.writebacked();
        writebackIter = this->storeQueue.erase(writebackIter);
        continue;
      } else {
        // The head of the store queue is not done.
        break;
      }
    }
  }
}

TDGLoadStoreQueue::LoadQueueEntry::LoadQueueEntry(
    const GemForgeLQCallback &_callback)
    : callback(_callback) {}

TDGLoadStoreQueue::StoreQueueEntry::StoreQueueEntry(
    const GemForgeSQCallback &_callback)
    : callback(_callback), committed(false), writebacking(false) {}