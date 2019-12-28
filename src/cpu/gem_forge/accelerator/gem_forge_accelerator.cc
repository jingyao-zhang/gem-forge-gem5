
#include "gem_forge_accelerator.hh"

// For the DPRINTF function.
#include "base/trace.hh"
#include "cpu/gem_forge/llvm_insts.hh"

// Include the accelerators.
#include "speculative_precomputation/speculative_precomputation_manager.hh"
#include "stream/stream_engine.hh"

void GemForgeAccelerator::handshake(GemForgeCPUDelegator *_cpuDelegator,
                                    GemForgeAcceleratorManager *_manager) {
  this->cpuDelegator = _cpuDelegator;
  this->manager = _manager;
}

GemForgeAcceleratorManager::GemForgeAcceleratorManager(
    GemForgeAcceleratorManagerParams *params)
    : SimObject(params), accelerators(params->accelerators),
      cpuDelegator(nullptr), tickEvent([this] { this->tick(); }, name()) {}

GemForgeAcceleratorManager::~GemForgeAcceleratorManager() {}

void GemForgeAcceleratorManager::addAccelerator(
    GemForgeAccelerator *accelerator) {
  this->accelerators.push_back(accelerator);
}

void GemForgeAcceleratorManager::handshake(
    GemForgeCPUDelegator *_cpuDelegator) {
  cpuDelegator = _cpuDelegator;
  for (auto accelerator : this->accelerators) {
    accelerator->handshake(_cpuDelegator, this);
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

void GemForgeAcceleratorManager::scheduleTickNextCycle() {
  if (!this->tickEvent.scheduled()) {
    // Schedule the next tick event.
    cpuDelegator->schedule(&tickEvent, Cycles(1));
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
  return nullptr;
}

SpeculativePrecomputationManager *
GemForgeAcceleratorManager::getSpeculativePrecomputationManager() {
  for (auto accelerator : this->accelerators) {
    if (auto spm =
            dynamic_cast<SpeculativePrecomputationManager *>(accelerator)) {
      return spm;
    }
  }
  panic("Failed to find the SpeculativePrecomputationManager.");
}

GemForgeAcceleratorManager *GemForgeAcceleratorManagerParams::create() {
  return new GemForgeAcceleratorManager(this);
}

void GemForgeAcceleratorManager::exitDump() {
  if (auto se = this->getStreamEngine()) {
    se->exitDump();
  }
}