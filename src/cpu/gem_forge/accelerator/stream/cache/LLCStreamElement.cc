#include "LLCStreamElement.hh"

#define DEBUG_TYPE LLCRubyStreamBase
#include "../stream_log.hh"

LLCStreamElement::LLCStreamElement(Stream *_S,
                                   const DynamicStreamId &_dynStreamId,
                                   uint64_t _idx, Addr _vaddr, int _size)
    : S(_S), dynStreamId(_dynStreamId), idx(_idx), size(_size), vaddr(_vaddr),
      readyBytes(0) {
  if (this->size > sizeof(this->value)) {
    panic("LLCStreamElement size overflow %d, %s.\n", this->size,
          this->dynStreamId);
  }
  this->value.fill(0);
}

uint64_t LLCStreamElement::getData(uint64_t streamId) const {
  assert(this->isReady());
  int32_t offset = 0;
  int size = this->size;
  this->S->getCoalescedOffsetAndSize(streamId, offset, size);
  assert(size <= sizeof(uint64_t) && "ElementSize overflow.");
  assert(offset + size <= this->size && "Size overflow.");
  return GemForgeUtils::rebuildData(this->getUInt8Ptr(offset), size);
}
