
#include "tdg_accelerator.hh"

// For the DPRINTF function.
#include "base/misc.hh"
#include "base/trace.hh"
#include "cpu/llvm_trace/llvm_insts.hh"
#include "debug/LLVMTraceCPU.hh"

// Include the accelerators.
#include "adfa/adfa.hh"

void TDGAccelerator::handshake(LLVMTraceCPU *_cpu,
                               TDGAcceleratorManager *_manager) {
  this->cpu = _cpu;
  this->manager = _manager;
}

TDGAcceleratorManager::TDGAcceleratorManager(
    TDGAcceleratorManagerParams *params)
    : SimObject(params) {
  this->addAccelerator(new AbstractDataFlowAccelerator());
}

TDGAcceleratorManager::~TDGAcceleratorManager() {
  for (auto accelerator : this->accelerators) {
    delete accelerator;
  }
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

void TDGAcceleratorManager::regStats() {
  SimObject::regStats();
  for (auto accelerator : this->accelerators) {
    accelerator->regStats();
  }
}

TDGAcceleratorManager *TDGAcceleratorManagerParams::create() {
  return new TDGAcceleratorManager(this);
}