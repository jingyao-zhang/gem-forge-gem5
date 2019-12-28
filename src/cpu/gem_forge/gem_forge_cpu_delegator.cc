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