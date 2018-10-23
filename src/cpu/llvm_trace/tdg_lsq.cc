#include "tdg_lsq.hh"

#include "llvm_iew_stage.hh"
#include "llvm_trace_cpu.hh"

#include "debug/TDGLoadStoreQueue.hh"

TDGLoadStoreQueue::TDGLoadStoreQueue(LLVMTraceCPU *_cpu, LLVMIEWStage *_iew,
                                     int _loadQueueSize, int _storeQueueSize)
    : cpu(_cpu), iew(_iew), loadQueueSize(_loadQueueSize),
      storeQueueSize(_storeQueueSize) {}

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
  auto inst = cpu->inflyInstMap.at(instId);
  inst->execute(cpu);
}
void TDGLoadStoreQueue::executeStore(LLVMDynamicInstId instId) {
  {
    bool found = false;
    for (auto id : this->storeQueue) {
      if (id == instId) {
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

void TDGLoadStoreQueue::commitLoad(LLVMDynamicInstId instId) {
  if (this->loadQueue.empty() || this->loadQueue.front() != instId) {
    panic("Failed to find the commit load instruction.");
  }
  this->loadQueue.pop_front();
}

void TDGLoadStoreQueue::commitStore(LLVMDynamicInstId instId) {
  if (this->storeQueue.empty() || this->storeQueue.front() != instId) {
    panic("Failed to find the commit store instruction.");
  }
  this->storeQueue.pop_front();
}