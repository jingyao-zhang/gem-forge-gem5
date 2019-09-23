#include "gem_forge_cpu_delegator.hh"

#include "mem/port_proxy.hh"

std::string GemForgeCPUDelegator::readStringFromMem(Addr vaddr) {
  PortProxy proxy(this->baseCPU->getSendFunctional(),
                  this->baseCPU->cacheLineSize());
  std::string s;
  uint8_t c;
  do {
    auto paddr = this->translateVAddrOracle(vaddr);
    proxy.readBlob(paddr, &c, 1);
    s.push_back(static_cast<char>(c));
    vaddr++;
  } while (c != 0);
  return s;
}
