#include "gem_forge_cpu_delegator.hh"

#include "mem/port_proxy.hh"
#include "params/BaseCPU.hh"

namespace gem5 {

std::string GemForgeCPUDelegator::readStringFromMem(Addr vaddr) {
  PortProxy proxy(dynamic_cast<RequestPort &>(this->baseCPU->getDataPort()),
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
  PortProxy proxy(dynamic_cast<RequestPort &>(this->baseCPU->getDataPort()),
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
  PortProxy proxy(dynamic_cast<RequestPort &>(this->baseCPU->getDataPort()),
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
  const auto &baseCPUParams = baseCPU->params();
  if (baseCPUParams.enableIdeaInorderCPU) {
    this->ideaInorderCPU = std::make_unique<GemForgeIdeaInorderCPU>(
        baseCPU->cpuId(), baseCPU->params().isa[0]->regClasses(), 4, true,
        true);
    this->ideaInorderCPUNoFUTiming = std::make_unique<GemForgeIdeaInorderCPU>(
        baseCPU->cpuId(), baseCPU->params().isa[0]->regClasses(), 4, false,
        false);
    this->ideaInorderCPUNoLDTiming = std::make_unique<GemForgeIdeaInorderCPU>(
        baseCPU->cpuId(), baseCPU->params().isa[0]->regClasses(), 4, true,
        false);
  }
  this->isaHandler = std::make_shared<GemForgeISAHandler>(this);

  if (baseCPUParams.enableIdeaCache) {
    // 256kB idea cache.
    this->ideaCache = std::make_unique<GemForgeIdeaCache>(256 * 1024);
  }
}

void GemForgeCPUDelegator::takeOverFrom(GemForgeCPUDelegator *oldDelegator) {
  // Take over the ISA handler.
  this->takeOverISAHandlerFrom(oldDelegator);
  // Take over the InstSeqNum.
  auto mySeqNum = this->getInstSeqNum();
  auto oldSeqNum = oldDelegator->getInstSeqNum();
  if (mySeqNum > oldSeqNum) {
    panic("%s-%d MySeqNum %llu >= %s-%d OldSeqNum %llu.\n",
          CPUTypeToString(this->cpuType), this->cpuId(), mySeqNum,
          CPUTypeToString(oldDelegator->cpuType), oldDelegator->cpuId(),
          oldSeqNum);
  }
  this->setInstSeqNum(oldSeqNum);
}

void GemForgeCPUDelegator::takeOverISAHandlerFrom(
    GemForgeCPUDelegator *oldDelegator) {
  // For now we just seize the pointer.
  this->isaHandler = oldDelegator->isaHandler;
  oldDelegator->isaHandler = nullptr;
  // Make sure it now has myself as the new delegator.
  this->isaHandler->takeOverBy(this);
}
} // namespace gem5
