#include "tdg_lsq.hh"

#include "llvm_iew_stage.hh"
#include "llvm_trace_cpu.hh"

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

void TDGLoadStoreQueue::executeLoad(LLVMDynamicInstId instId) {}
void TDGLoadStoreQueue::executeStore(LLVMDynamicInstId instId) {}

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