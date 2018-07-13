
#include "tdg_accelerator.hh"

// For the DPRINTF function.
#include "base/misc.hh"
#include "base/trace.hh"
#include "cpu/llvm_trace/llvm_insts.hh"
#include "debug/LLVMTraceCPU.hh"

// Include the accelerators.
#include "adfa/adfa.hh"

void TDGAccelerator::handshake(LLVMTraceCPU *_cpu) { this->cpu = _cpu; }

TDGAcceleratorManager::TDGAcceleratorManager() {
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
    accelerator->handshake(_cpu);
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