#include "tdg_lsq.hh"

#include "llvm_iew_stage.hh"
#include "llvm_trace_cpu.hh"

#include "debug/TDGLoadStoreQueue.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

TDGLoadStoreQueue::TDGLoadStoreQueue(LLVMTraceCPU *_cpu, LLVMIEWStage *_iew,
                                     int _loadQueueSize, int _storeQueueSize,
                                     int _cacheStorePorts)
    : cpu(_cpu), iew(_iew), loadQueueSize(_loadQueueSize),
      storeQueueSize(_storeQueueSize), cacheStorePorts(_cacheStorePorts) {}

void TDGLoadStoreQueue::insertLoad(LLVMDynamicInstId instId) {
  panic_if(this->loadQueue.size() > this->loadQueueSize,
           "Load queue overflows.");
  this->loadQueue.emplace_back(instId);
}

void TDGLoadStoreQueue::insertStore(LLVMDynamicInstId instId) {
  if (this->storeQueue.size() > this->storeQueueSize) {
    panic("Store queue overflows.");
  }
  this->storeQueue.emplace_back(instId);
}

void TDGLoadStoreQueue::executeLoad(LLVMDynamicInstId instId) {
  {
    bool found = false;
    for (auto id : this->loadQueue) {
      if (id == instId) {
        found = true;
        break;
      }
    }
    if (!found) {
      panic("Failed to find the load inst %d in the load queue.", instId);
    }
  }
  auto instStatus = cpu->inflyInstStatus.at(instId);
  panic_if(instStatus != InstStatus::ISSUED,
           "Should be issued to execute the load.");
  if (cpu->dataPort.isBlocked()) {
    // We are blocked.
    iew->blockMemInst(instId);
    return;
  }
  auto inst = cpu->inflyInstMap.at(instId);
  inst->execute(cpu);
}

void TDGLoadStoreQueue::executeStore(LLVMDynamicInstId instId) {
  {
    bool found = false;
    for (const auto &entry : this->storeQueue) {
      if (entry.id == instId) {
        found = true;
        break;
      }
    }
    if (!found) {
      panic("Failed to find the store inst %d in the store queue.", instId);
    }
  }
  auto inst = cpu->inflyInstMap.at(instId);
  inst->execute(cpu);
}

void TDGLoadStoreQueue::postCommitLoad(LLVMDynamicInstId instId) {
  if (this->loadQueue.empty() || this->loadQueue.front() != instId) {
    panic("Failed to find the commit load instruction.");
  }
  this->loadQueue.pop_front();
}

void TDGLoadStoreQueue::postCommitStore(LLVMDynamicInstId instId) {}

void TDGLoadStoreQueue::commitLoad(LLVMDynamicInstId instId) {}

void TDGLoadStoreQueue::commitStore(LLVMDynamicInstId instId) {
  auto commitIter = this->storeQueue.begin();
  while (commitIter != this->storeQueue.end() && commitIter->committed) {
    commitIter++;
  }
  if (commitIter == this->storeQueue.end()) {
    panic("Try to commit store %lu when there is no instruction in the store "
          "queue waiting to be committed.",
          instId);
  }
  if (commitIter->id != instId) {
    panic("Unmatched commit store instruction id %lu.", commitIter->id);
  }
  DPRINTF(TDGLoadStoreQueue, "Commit store %lu.\n", instId);
  commitIter->committed = true;
}

void TDGLoadStoreQueue::writebackStore() {
  auto writebackIter = this->storeQueue.begin();
  auto usedStorePorts = 0;
  while (writebackIter != this->storeQueue.end() && writebackIter->committed) {
    auto instId = writebackIter->id;
    panic_if(cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end(),
             "Failed to find the status for writeback store instruction %lu.",
             instId);
    auto instStatus = cpu->inflyInstStatus.at(instId);
    auto inst = cpu->inflyInstMap.at(instId);
    DPRINTF(TDGLoadStoreQueue, "Committed store %lu, status %d.\n", instId,
            instStatus);
    if (!writebackIter->writebacking) {
      // We start to writeback it.
      DPRINTF(TDGLoadStoreQueue, "Store inst %u is started to writeback.\n",
              instId);
      writebackIter->writebacking = true;
      inst->writeback(cpu);
      usedStorePorts++;
      if (usedStorePorts >= this->cacheStorePorts) {
        // We are done for this cycle.
        break;
      }

    } else {

      // We should check if it is writebacked.
      if (inst->isWritebacked()) {
        // If so, update it to writebacked and remove from the store queue.
        // Continue to try to write back the next store in queue.
        DPRINTF(TDGLoadStoreQueue, "Store inst %u is writebacked.\n", instId);
        cpu->inflyInstStatus.at(instId) = InstStatus::COMMITTED;
        writebackIter = this->storeQueue.erase(writebackIter);
        continue;
      } else {
        // The head of the store queue is not done.
        // break.
        break;
      }
    }
  }
}

TDGLoadStoreQueue::StoreQueueEntry::StoreQueueEntry(LLVMDynamicInstId _id)
    : id(_id), committed(false), writebacking(false) {}