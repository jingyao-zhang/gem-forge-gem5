#include "LLCDynamicStream.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

// TODO: Support real flow control.
LLCDynamicStream::LLCDynamicStream(CacheStreamConfigureData *_configData)
    : configData(*_configData), slicedStream(_configData),
      maxWaitingDataBaseRequests(2), sliceIdx(0),
      allocatedSliceIdx(_configData->initAllocatedIdx),
      waitingDataBaseRequests(0) {
  if (this->configData.isPointerChase) {
    // Pointer chase stream can only have at most one base requests waiting for
    // data.
    this->maxWaitingDataBaseRequests = 1;
  }
}

LLCDynamicStream::~LLCDynamicStream() {
  for (auto &indirectStream : this->indirectStreams) {
    delete indirectStream;
    indirectStream = nullptr;
  }
  this->indirectStreams.clear();
}

Addr LLCDynamicStream::peekVAddr() {
  return this->slicedStream.peekNextSlice().vaddr;
}

Addr LLCDynamicStream::getVAddr(uint64_t sliceIdx) const {
  panic("getVAddr is deprecated.\n");
  return 0;
}

Addr LLCDynamicStream::translateToPAddr(Addr vaddr) const {
  // ! Do something reasonable here to translate the vaddr.
  auto cpuDelegator = this->configData.stream->getCPUDelegator();
  Addr paddr;
  if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
    panic("Failed translate vaddr %#x.\n", vaddr);
  }
  return paddr;
}

void LLCDynamicStream::addCredit(uint64_t n) {
  this->allocatedSliceIdx += n;
  for (auto indirectStream : this->indirectStreams) {
    indirectStream->addCredit(n);
  }
}