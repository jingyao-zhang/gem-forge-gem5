#ifndef __CPU_TDG_ACCELERATOR_LLC_STREAM_ELEMENT_H__
#define __CPU_TDG_ACCELERATOR_LLC_STREAM_ELEMENT_H__

#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include <memory>

struct LLCStreamElement;
using LLCStreamElementPtr = std::shared_ptr<LLCStreamElement>;
using ConstLLCStreamElementPtr = std::shared_ptr<const LLCStreamElement>;

struct LLCStreamElement {
  /**
   * This represents the basic unit of LLCStreamElement.
   * It remembers the base elements it depends on. Since this can be a
   * remote LLCDynamicStream sending here, we do not remember LLCDynamicStream
   * in the element, but just the DynamicStreamId and the StaticStream.
   */
  LLCStreamElement(Stream *_S, const DynamicStreamId &_dynStreamId,
                   uint64_t _idx, Addr _vaddr, int _size);

  Stream *S;
  const DynamicStreamId &dynStreamId;
  const uint64_t idx;
  const int size;
  Addr vaddr = 0;

  std::vector<LLCStreamElementPtr> baseElements;
  bool areBaseElementsReady() const {
    for (const auto &baseElement : this->baseElements) {
      if (!baseElement->isReady()) {
        return false;
      }
    }
    return true;
  }

  // This is the final value.
  int readyBytes;
  StreamValue value;

  bool isReady() const { return this->readyBytes == this->size; }

  /*************************************************
   * Accessors to the data.
   *************************************************/
  uint64_t getUInt64() const {
    assert(this->isReady());
    assert(this->size <= sizeof(uint64_t));
    return this->value.front();
  }
  uint64_t getData(uint64_t streamId) const;

  const StreamValue &getValue() const { return this->value; }
  StreamValue &getValue() { return this->value; }

  uint8_t *getUInt8Ptr(int offset = 0) {
    assert(offset < this->size);
    return this->value.uint8Ptr(offset);
  }
  const uint8_t *getUInt8Ptr(int offset = 0) const {
    assert(offset < this->size);
    return this->value.uint8Ptr(offset);
  }
};

#endif