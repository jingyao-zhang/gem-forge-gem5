#include "LLCDynamicStream.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

// TODO: Support real flow control.
LLCDynamicStream::LLCDynamicStream(CacheStreamConfigureData *_configData)
    : configData(*_configData), idx(0),
      allocatedIdx(_configData->initAllocatedIdx) {}

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
    // ! Do something reasonable here.
    return 0;
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