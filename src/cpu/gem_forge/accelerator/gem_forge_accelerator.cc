
#include "gem_forge_accelerator.hh"

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

void GemForgeAccelerator::handshake(LLVMTraceCPU *_cpu,
                                    GemForgeAcceleratorManager *_manager) {
  this->cpu = _cpu;
  this->manager = _manager;
}

GemForgeAcceleratorManager::GemForgeAcceleratorManager(
    GemForgeAcceleratorManagerParams *params)
    : SimObject(params), accelerators(params->accelerators) {
  // this->addAccelerator(new AbstractDataFlowAccelerator());
  // this->addAccelerator(new StreamEngine());
  // this->addAccelerator(new SpeculativePrecomputationManager());
  // this->addAccelerator(new IdealPrefetcher());
}

GemForgeAcceleratorManager::~GemForgeAcceleratorManager() {}

void GemForgeAcceleratorManager::addAccelerator(
    GemForgeAccelerator *accelerator) {
  this->accelerators.push_back(accelerator);
}

void GemForgeAcceleratorManager::handshake(LLVMTraceCPU *_cpu) {
  for (auto accelerator : this->accelerators) {
    accelerator->handshake(_cpu, this);
  }
}

void GemForgeAcceleratorManager::handle(LLVMDynamicInst *inst) {
  for (auto accelerator : this->accelerators) {
    if (accelerator->handle(inst)) {
      return;
    }
  }
  panic("Unable to handle accelerator instruction id %u.", inst->getId());
}

void GemForgeAcceleratorManager::tick() {
  for (auto accelerator : this->accelerators) {
    accelerator->tick();
  }
}

void GemForgeAcceleratorManager::dump() {
  for (auto accelerator : this->accelerators) {
    accelerator->dump();
  }
}

void GemForgeAcceleratorManager::regStats() { SimObject::regStats(); }

StreamEngine *GemForgeAcceleratorManager::getStreamEngine() {
  for (auto accelerator : this->accelerators) {
    if (auto se = dynamic_cast<StreamEngine *>(accelerator)) {
      return se;
    }
  }
  panic("Failed to find the stream engine to handle commitStreamStore.");
}

SpeculativePrecomputationManager *
GemForgeAcceleratorManager::getSpeculativePrecomputationManager() {
  for (auto accelerator : this->accelerators) {
    if (auto spm =
            dynamic_cast<SpeculativePrecomputationManager *>(accelerator)) {
      return spm;
    }
  }
  panic("Failed to find the stream engine to handle commitStreamStore.");
}

GemForgeAcceleratorManager *GemForgeAcceleratorManagerParams::create() {
  return new GemForgeAcceleratorManager(this);
}

void GemForgeAcceleratorManager::exitDump() {
  if (auto se = this->getStreamEngine()) {
    se->exitDump();
  }
}