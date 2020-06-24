#include "gem_forge_cpu_delegator.hh"

#include "mem/port_proxy.hh"
#include "params/BaseCPU.hh"

std::string GemForgeCPUDelegator::readStringFromMem(Addr vaddr) {
  PortProxy proxy(this->baseCPU->getSendFunctional(),
                  this->baseCPU->cacheLineSize());
  std::string s;
  uint8_t c;
  do {
    Addr paddr;
    if (!this->translateVAddrOracle(vaddr, paddr)) {
      panic("Failed translate vaddr %#x.\n", vaddr);
    }
    proxy.readBlob(paddr, &c, 1);
    s.push_back(static_cast<char>(c));
    vaddr++;
  } while (c != 0);
  return s;
}

void GemForgeCPUDelegator::readFromMem(Addr vaddr, int size, uint8_t *data) {
  PortProxy proxy(this->baseCPU->getSendFunctional(),
                  this->baseCPU->cacheLineSize());
  for (int i = 0; i < size; ++i) {
    Addr paddr;
    if (!this->translateVAddrOracle(vaddr + i, paddr)) {
      panic("Failed translate vaddr %#x.\n", vaddr + i);
    }
    proxy.readBlob(paddr, data + i, 1);
  }
}

void GemForgeCPUDelegator::writeToMem(Addr vaddr, int size,
                                      const uint8_t *data) {
  PortProxy proxy(this->baseCPU->getSendFunctional(),
                  this->baseCPU->cacheLineSize());
  for (int i = 0; i < size; ++i) {
    Addr paddr;
    if (!this->translateVAddrOracle(vaddr + i, paddr)) {
      panic("Failed translate vaddr %#x.\n", vaddr + i);
    }
    proxy.writeBlob(paddr, data + i, 1);
  }
}

GemForgeCPUDelegator::GemForgeCPUDelegator(CPUTypeE _cpuType, BaseCPU *_baseCPU)
    : cpuType(_cpuType), baseCPU(_baseCPU) {
  auto baseCPUParams = dynamic_cast<const BaseCPUParams *>(baseCPU->params());
  if (baseCPUParams->enableIdeaInorderCPU) {
    this->ideaInorderCPU = m5::make_unique<GemForgeIdeaInorderCPU>(
        baseCPU->cpuId(), 4, true, true);
    this->ideaInorderCPUNoFUTiming = m5::make_unique<GemForgeIdeaInorderCPU>(
        baseCPU->cpuId(), 4, false, false);
    this->ideaInorderCPUNoLDTiming = m5::make_unique<GemForgeIdeaInorderCPU>(
        baseCPU->cpuId(), 4, true, false);
  }
}