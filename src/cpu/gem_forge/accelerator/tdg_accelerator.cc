
#include "tdg_accelerator.hh"

// For the DPRINTF function.
// #include "base/misc.hh""
#include "base/trace.hh"
#include "cpu/gem_forge/llvm_insts.hh"
#include "debug/LLVMTraceCPU.hh"

// Include the accelerators.
#include "adfa/adfa.hh"
#include "ideal_prefetcher/ideal_prefetcher.hh"
#include "speculative_precomputation/speculative_precomputation_manager.hh"
#include "stream/stream_engine.hh"

void TDGAccelerator::handshake(LLVMTraceCPU *_cpu,
                               TDGAcceleratorManager *_manager) {
  this->cpu = _cpu;
  this->manager = _manager;
}

TDGAcceleratorManager::TDGAcceleratorManager(
    TDGAcceleratorManagerParams *params)
    : SimObject(params) {
  this->addAccelerator(new AbstractDataFlowAccelerator());
  this->addAccelerator(new StreamEngine());
  this->addAccelerator(new SpeculativePrecomputationManager());
  this->addAccelerator(new IdealPrefetcher());
}

TDGAcceleratorManager::~TDGAcceleratorManager() {
  for (auto &accelerator : this->accelerators) {
    delete accelerator;
    accelerator = nullptr;
  }
  this->accelerators.clear();
}

void TDGAcceleratorManager::addAccelerator(TDGAccelerator *accelerator) {
  this->accelerators.push_back(accelerator);
}

void TDGAcceleratorManager::handshake(LLVMTraceCPU *_cpu) {
  for (auto accelerator : this->accelerators) {
    accelerator->handshake(_cpu, this);
  }
}

void TDGAcceleratorManager::handle(LLVMDynamicInst *inst) {
  for (auto accelerator : this->accelerators) {
    if (accelerator->handle(inst)) {
      return;
    }
  }
  panic("Unable to handle accelerator instruction id %u.", inst->getId());
}

void TDGAcceleratorManager::tick() {
  for (auto accelerator : this->accelerators) {
    accelerator->tick();
  }
}

void TDGAcceleratorManager::dump() {
  for (auto accelerator : this->accelerators) {
    accelerator->dump();
  }
}

void TDGAcceleratorManager::regStats() {
  SimObject::regStats();
  for (auto accelerator : this->accelerators) {
    accelerator->regStats();
  }
}

StreamEngine *TDGAcceleratorManager::getStreamEngine() {
  for (auto accelerator : this->accelerators) {
    if (auto se = dynamic_cast<StreamEngine *>(accelerator)) {
      return se;
    }
  }
  panic("Failed to find the stream engine to handle commitStreamStore.");
}

SpeculativePrecomputationManager *
TDGAcceleratorManager::getSpeculativePrecomputationManager() {
  for (auto accelerator : this->accelerators) {
    if (auto spm =
            dynamic_cast<SpeculativePrecomputationManager *>(accelerator)) {
      return spm;
    }
  }
  panic("Failed to find the stream engine to handle commitStreamStore.");
}

TDGAcceleratorManager *TDGAcceleratorManagerParams::create() {
  return new TDGAcceleratorManager(this);
}

void TDGAcceleratorManager::exitDump() {
  if (auto se = this->getStreamEngine()) {
    se->exitDump();
  }
}