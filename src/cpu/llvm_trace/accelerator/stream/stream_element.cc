#include "stream_element.hh"

#include "cpu/llvm_trace/llvm_trace_cpu.hh"

FIFOEntryIdx::FIFOEntryIdx()
    : streamInstance(0), configSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
      entryIdx(0) {}

FIFOEntryIdx::FIFOEntryIdx(uint64_t _streamInstance, uint64_t _configSeqNum,
                           uint64_t _entryIdx)
    : streamInstance(_streamInstance), configSeqNum(_configSeqNum),
      entryIdx(_entryIdx) {}

StreamElement::StreamElement() { this->clear(); }

void StreamElement::clear() {
  this->baseElements.clear();
  this->next = nullptr;
  this->stream = nullptr;
  this->FIFOIdx = FIFOEntryIdx();
  this->isAddrReady = false;
  this->isValueReady = false;

  this->allocateCycle = Cycles(0);
  this->valueReadyCycle = Cycles(0);
  this->firstCheckCycle = Cycles(0);

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
    this->valueReadyCycle = cpu->curCycle();
  }
}

void StreamElement::dump() const {}