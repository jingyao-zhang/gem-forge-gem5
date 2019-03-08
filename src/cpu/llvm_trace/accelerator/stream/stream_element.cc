#include "stream_element.hh"

StreamElement::StreamElement() { this->clear(); }

void StreamElement::clear() {
  this->baseElements.clear();
  this->next = nullptr;
  this->stream = nullptr;
  this->isAddrReady = false;
  this->isValueReady = false;

  this->allocateCycle = Cycles(0);

  this->cacheBlocks = 0;
  this->size = 0;
  this->addr = 0;
  this->inflyLoadPackets.clear();
  this->stored = false;
}

void StreamElement::handlePacketResponse(LLVMTraceCPU *cpu, PacketPtr packet) {
  if (this->inflyLoadPackets.count(packet) == 0) {
    return;
  }
  this->inflyLoadPackets.erase(packet);
  if (this->inflyLoadPackets.size() == 0) {
    this->isValueReady = true;
  }
}

void StreamElement::dump() const {}