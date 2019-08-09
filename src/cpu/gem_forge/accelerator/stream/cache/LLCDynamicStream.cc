#include "LLCDynamicStream.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

// TODO: Support real flow control.
LLCDynamicStream::LLCDynamicStream(CacheStreamConfigureData *_configData)
    : configData(*_configData), idx(0),
      allocatedIdx(_configData->initAllocatedIdx) {}

Addr LLCDynamicStream::peekVAddr() const {
  auto historySize = this->configData.history->history_size();
  if (this->idx < historySize) {
    auto vaddr = this->configData.history->history(this->idx).addr();
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