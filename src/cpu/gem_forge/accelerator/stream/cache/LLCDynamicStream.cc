#include "LLCDynamicStream.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

// TODO: Support real flow control.
LLCDynamicStream::LLCDynamicStream(CacheStreamConfigureData *_configData)
    : configData(*_configData), maxWaitingDataBaseRequests(2), idx(0),
      allocatedIdx(_configData->initAllocatedIdx), waitingDataBaseRequests(0) {
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

Addr LLCDynamicStream::peekVAddr() const { return this->getVAddr(this->idx); }

Addr LLCDynamicStream::getVAddr(uint64_t idx) const {
  auto historySize = this->configData.history->history_size();
  if (idx < historySize) {
    auto vaddr = this->configData.history->history(idx).addr();
    return vaddr;
  } else {
    // ! So far just return the last address.
    // ! Do something reasonable here.
    // ! Make sure this is consistent with the core stream engine.
    return this->configData.history->history(historySize - 1).addr();
  }
}

Addr LLCDynamicStream::translateToPAddr(Addr vaddr) const {
  // ! Do something reasonable here to translate the vaddr.
  auto cpu = this->configData.stream->getCPU();
  return cpu->translateAndAllocatePhysMem(vaddr);
}

void LLCDynamicStream::addCredit(uint64_t n) {
  this->allocatedIdx += n;
  for (auto indirectStream : this->indirectStreams) {
    indirectStream->addCredit(n);
  }
}