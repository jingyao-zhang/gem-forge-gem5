#include "LLCStreamElement.hh"

#include "mem/simple_mem.hh"

#include "LLCDynamicStream.hh"

#include "debug/LLCRubyStreamBase.hh"
#include "debug/StreamRangeSync.hh"
#define DEBUG_TYPE LLCRubyStreamBase
#include "../stream_log.hh"

LLCStreamElement::LLCStreamElement(
    Stream *_S, AbstractStreamAwareController *_mlcController,
    const DynamicStreamId &_dynStreamId, uint64_t _idx, Addr _vaddr, int _size,
    bool _isNDCElement)
    : S(_S), mlcController(_mlcController), dynStreamId(_dynStreamId),
      idx(_idx), size(_size), isNDCElement(_isNDCElement), vaddr(_vaddr),
      readyBytes(0) {
  if (this->size > sizeof(this->value)) {
    panic("LLCStreamElement size overflow %d, %s.\n", this->size,
          this->dynStreamId);
  }
  if (!this->mlcController) {
    panic("LLCStreamElement allocated without MLCController.\n");
  }
  this->value.fill(0);
}

LLCStreamElement::~LLCStreamElement() {
  this->S->statistic.sampleLLCElement(this->firstCheckCycle,
                                      this->valueReadyCycle);
}

int LLCStreamElement::curRemoteBank() const {
  /**
   * So far we don't have a good definition of the current LLC bank for an
   * element.
   */
  return -1;
}

const char *LLCStreamElement::curRemoteMachineType() const {
  /**
   * So far we don't have a good definition of the current remote bank for an
   * element.
   */
  return "XXX";
}

StreamValue LLCStreamElement::getValue(int offset, int size) const {
  if (this->size < offset + size) {
    LLC_ELEMENT_PANIC(
        this,
        "Try to get StreamValue (offset %d size %d) for LLCStreamElement of "
        "size %d.",
        offset, size, this->size);
  }
  StreamValue v;
  memcpy(v.uint8Ptr(), this->getUInt8Ptr(offset), size);
  return v;
}

StreamValue LLCStreamElement::getBaseStreamValue(uint64_t baseStreamId) {
  for (const auto &baseE : this->baseElements) {
    if (baseE->S->isCoalescedHere(baseStreamId)) {
      // Found it.
      return baseE->getValueByStreamId(baseStreamId);
    }
  }
  LLC_ELEMENT_PANIC(this, "Invalid baseStreamId %llu.", baseStreamId);
  return StreamValue();
}

StreamValue LLCStreamElement::getBaseOrMyStreamValue(uint64_t streamId) {
  if (this->S->isCoalescedHere(streamId)) {
    // This is from myself.
    return this->getValueByStreamId(streamId);
  } else {
    // This is from a value base stream.
    return this->getBaseStreamValue(streamId);
  }
}

uint8_t *LLCStreamElement::getUInt8Ptr(int offset) {
  assert(offset < this->size);
  return reinterpret_cast<uint8_t *>(this->value.data()) + offset;
}

const uint8_t *LLCStreamElement::getUInt8Ptr(int offset) const {
  assert(offset < this->size);
  return reinterpret_cast<const uint8_t *>(this->value.data()) + offset;
}

StreamValue LLCStreamElement::getValueByStreamId(uint64_t streamId) const {
  assert(this->isReady());
  int32_t offset = 0;
  int size = this->size;
  this->S->getCoalescedOffsetAndSize(streamId, offset, size);
  return this->getValue(offset, size);
}

uint64_t LLCStreamElement::getUInt64ByStreamId(uint64_t streamId) const {
  assert(this->isReady());
  int32_t offset = 0;
  int size = this->size;
  this->S->getCoalescedOffsetAndSize(streamId, offset, size);
  assert(size <= sizeof(uint64_t) && "ElementSize overflow.");
  assert(offset + size <= this->size && "Size overflow.");
  return GemForgeUtils::rebuildData(this->getUInt8Ptr(offset), size);
}

void LLCStreamElement::setValue(const StreamValue &value) {
  assert(this->readyBytes == 0 && "Already ready.");
  if (this->size > sizeof(StreamValue)) {
    panic("Try to set StreamValue for LLCStreamElement of size %d.",
          this->size);
  }
  memcpy(this->getUInt8Ptr(), value.uint8Ptr(), this->size);
  this->readyBytes += this->size;
  if (this->isReady()) {
    this->valueReadyCycle = this->mlcController->curCycle();
  }
}

void LLCStreamElement::setComputedValue(const StreamValue &value) {
  assert(!this->computedValueReady && "ComputedValue already ready.");
  this->computedValue = value;
  this->computedValueReady = true;
  this->valueReadyCycle = this->mlcController->curCycle();
}

int LLCStreamElement::computeOverlap(Addr rangeVAddr, int rangeSize,
                                     int &rangeOffset,
                                     int &elementOffset) const {
  if (this->vaddr == 0) {
    panic("Try to computeOverlap without elementVAddr.");
  }
  // Compute the overlap between the element and the slice.
  Addr overlapLHS = std::max(this->vaddr, rangeVAddr);
  Addr overlapRHS = std::min(this->vaddr + this->size, rangeVAddr + rangeSize);
  // Check that the overlap is within the same line.
  assert(overlapLHS < overlapRHS && "Empty overlap.");
  assert(makeLineAddress(overlapLHS) == makeLineAddress(overlapRHS - 1) &&
         "Illegal overlap.");
  auto overlapSize = overlapRHS - overlapLHS;
  rangeOffset = overlapLHS - rangeVAddr;
  elementOffset = overlapLHS - this->vaddr;
  return overlapSize;
}

void LLCStreamElement::extractElementDataFromSlice(
    GemForgeCPUDelegator *cpuDelegator, const DynamicStreamSliceId &sliceId,
    const DataBlock &dataBlock) {
  /**
   * Extract the element data and update the LLCStreamElement.
   */
  auto elementIdx = this->idx;
  auto elementSize = this->size;
  if (this->vaddr == 0) {
    LLC_ELEMENT_PANIC(this, "Cannot extract data without vaddr.");
  }

  int sliceOffset;
  int elementOffset;
  int overlapSize = this->computeOverlap(sliceId.vaddr, sliceId.getSize(),
                                         sliceOffset, elementOffset);
  assert(overlapSize > 0 && "Empty overlap.");
  Addr overlapLHS = this->vaddr + elementOffset;

  LLC_SLICE_DPRINTF(
      sliceId, "Received element %lu size %d Overlap [%lu, %lu).\n", elementIdx,
      elementSize, elementOffset, elementOffset + overlapSize);

  // Get the data from the cache line.
  auto data = dataBlock.getData(overlapLHS % RubySystem::getBlockSizeBytes(),
                                overlapSize);
  memcpy(this->getUInt8Ptr(elementOffset), data, overlapSize);

  // Mark these bytes ready.
  this->readyBytes += overlapSize;
  if (this->readyBytes > this->size) {
    LLC_SLICE_PANIC(
        sliceId,
        "Too many ready bytes %lu Overlap [%lu, %lu), ready %d > size %d.",
        elementIdx, elementOffset, elementOffset + overlapSize,
        this->readyBytes, this->size);
  }
  if (this->isReady()) {
    this->valueReadyCycle = this->mlcController->curCycle();
  }
}

void LLCStreamElement::addSlice(LLCStreamSlicePtr &slice) {
  if (this->numSlices >= MAX_SLICES_PER_ELEMENT) {
    LLC_SLICE_PANIC(slice->getSliceId(), "Element -> Slices overflow.");
  }
  LLC_ELEMENT_DPRINTF(this, "Register slice %s.\n", slice->getSliceId());
  this->slices[this->numSlices] = slice;
  this->numSlices++;
}