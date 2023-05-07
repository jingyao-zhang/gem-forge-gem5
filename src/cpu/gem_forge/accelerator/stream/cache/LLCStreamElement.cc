#include "LLCStreamElement.hh"

#include "mem/simple_mem.hh"

#include "LLCDynStream.hh"
#include "LLCStreamEngine.hh"

#include "debug/LLCRubyStreamBase.hh"
#include "debug/LLCStreamSample.hh"
#include "debug/StreamRangeSync.hh"
#define DEBUG_TYPE LLCRubyStreamBase
#include "../stream_log.hh"

namespace gem5 {

std::list<LLCStreamElementPtr> LLCStreamElement::deferredReleaseElems;

uint64_t LLCStreamElement::aliveElems = 0;

LLCStreamElement::LLCStreamElement(
    Stream *_S, ruby::AbstractStreamAwareController *_mlcController,
    const DynStrandId &_strandId, uint64_t _idx, Addr _vaddr, int _size,
    bool _isNDCElement)
    : S(_S), mlcController(_mlcController), strandId(_strandId), idx(_idx),
      size(_size), isNDCElement(_isNDCElement), vaddr(_vaddr), readyBytes(0) {
  if (this->size > sizeof(this->value)) {
    panic("LLCStreamElem size overflow %d, %s.\n", this->size, this->strandId);
  }
  if (!this->mlcController) {
    panic("LLCStreamElem allocated without MLCController.\n");
  }
  LLCStreamElement::aliveElems++;
  this->value.fill(0);
  if (deferredReleaseElems.size() > 100) {
    releaseDeferredElements();
  }
}

LLCStreamElement::~LLCStreamElement() {

  StreamStatistic::LLCElementSample s = {
      this->firstCheckCycle, this->readyToIssueCycle, this->issueCycle,
      this->valueReadyCycle};

  if (this->valueReadyCycle > this->issueCycle && this->valueReadyCycle != 0) {
    LLC_ELEMENT_DPRINTF_(LLCStreamSample, this,
                         "[Sample] LLC Elem ReqIssue -> ValueReady %lu.\n",
                         this->valueReadyCycle - this->issueCycle);
  }

  this->S->statistic.sampleLLCElement(s);
  LLCStreamElement::aliveElems--;
  if (this->prevReduceElem) {
    deferredReleaseElems.emplace_back(std::move(this->prevReduceElem));
  }
  while (!this->baseElements.empty()) {
    deferredReleaseElems.emplace_back(std::move(this->baseElements.back()));
    this->baseElements.pop_back();
  }
}

void LLCStreamElement::releaseDeferredElements() {
  while (!deferredReleaseElems.empty()) {
    std::list<LLCStreamElementPtr> tmp;
    std::swap(tmp, deferredReleaseElems);
    tmp.clear();
  }
}

int LLCStreamElement::curRemoteBank() const {
  /**
   * So far we don't have a good definition of the current LLC bank for an
   * element.
   */
  if (this->llcSE) {
    return this->llcSE->curRemoteBank();
  }
  return -1;
}

const char *LLCStreamElement::curRemoteMachineType() const {
  /**
   * So far we don't have a good definition of the current remote bank for an
   * element.
   */
  if (this->llcSE) {
    return this->llcSE->curRemoteMachineType();
  }
  return "XXX";
}

StreamValue LLCStreamElement::getValue(int offset, int size) const {
  if (this->size < offset + size) {
    LLC_ELEMENT_PANIC(this,
                      "Try to get Value (offset %d size %d) for Elem size %d.",
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
      return baseE->getUsedValueByStreamId(baseStreamId);
    }
  }
  LLC_ELEMENT_PANIC(this, "Invalid BaseStreamId %llu.", baseStreamId);
  return StreamValue();
}

StreamValue LLCStreamElement::getUsedBaseOrMyStreamValue(uint64_t streamId) {
  if (this->S->isCoalescedHere(streamId)) {
    // This is from myself.
    return this->getUsedValueByStreamId(streamId);
  } else {
    // This is from a value base stream.
    return this->getBaseStreamValue(streamId);
  }
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

StreamValue LLCStreamElement::getUsedValueByStreamId(uint64_t streamId) const {

  if (this->S->isLoadComputeStream()) {
    // This is the ComputedValue.
    assert(this->isComputedValueReady() && "ComputedValue not Ready.");
    int32_t offset = 0;
    int size = this->size;
    this->S->getCoalescedOffsetAndSize(streamId, offset, size);
    assert(offset == 0);
    assert(size == this->size);
    return this->computedValue;
  }

  return this->getValueByStreamId(streamId);
}

StreamValue LLCStreamElement::getValueByStreamId(uint64_t streamId) const {
  if (!this->isReady()) {
    LLC_ELEMENT_PANIC(this, "GetValueByStreamId but NotReady.");
  }
  int32_t offset = 0;
  int size = this->size;
  // AtomicComputeS is never coalesced, and should use CoreElemSize.
  if (!this->S->isAtomicComputeStream()) {
    this->S->getCoalescedOffsetAndSize(streamId, offset, size);
  }
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
  this->addReadyBytes(this->size);
  assert(this->isReady());
  this->valueReadyCycle = this->mlcController->curCycle();
}

void LLCStreamElement::setValue(uint64_t value) {
  assert(this->readyBytes == 0 && "Already ready.");
  if (this->size > sizeof(value)) {
    panic("Try to set UINT64 for LLCStreamElement of size %d.", this->size);
  }
  memcpy(this->getUInt8Ptr(), reinterpret_cast<uint8_t *>(&value), this->size);
  this->addReadyBytes(this->size);
  assert(this->isReady());
  this->valueReadyCycle = this->mlcController->curCycle();
}

void LLCStreamElement::setComputedValue(const StreamValue &value) {
  assert(!this->computedValueReady && "ComputedValue already ready.");
  this->computedValue = value;
  this->computedValueReady = true;
  this->valueReadyCycle = this->mlcController->curCycle();
}

bool LLCStreamElement::isLastSlice(const DynStreamSliceId &sliceId) const {
  int sliceOffset;
  int elemOffset;
  int overlapSize = this->computeOverlap(sliceId.vaddr, sliceId.getSize(),
                                         sliceOffset, elemOffset);
  assert(overlapSize > 0 && "Empty overlap.");
  if (elemOffset + overlapSize == this->size) {
    // This slice contains the last element byte.
    return true;
  } else {
    return false;
  }
}

int LLCStreamElement::computeOverlap(Addr rangeVAddr, int rangeSize,
                                     int &rangeOffset,
                                     int &elementOffset) const {
  auto overlapSize = this->computeOverlapImpl(this->size, rangeVAddr, rangeSize,
                                              rangeOffset, elementOffset);
  assert(overlapSize > 0 && "Empty overlap.");
  return overlapSize;
}

int LLCStreamElement::computeLoadComputeOverlap(Addr rangeVAddr, int rangeSize,
                                                int &rangeOffset,
                                                int &elementOffset) const {
  /**
   * LoadCompute may have empty overlap due to shrinked CoreElemSize.
   */
  return this->computeOverlapImpl(this->S->getCoreElementSize(), rangeVAddr,
                                  rangeSize, rangeOffset, elementOffset);
}

int LLCStreamElement::computeOverlapImpl(int elemSize, Addr rangeVAddr,
                                         int rangeSize, int &rangeOffset,
                                         int &elemOffset) const {
  if (this->S->isMemStream()) {
    if (this->vaddr == 0) {
      panic("Try to computeOverlap without elementVAddr.");
    }
  }
  // Compute the overlap between the element and the slice.
  Addr overlapLHS = std::max(this->vaddr, rangeVAddr);
  Addr overlapRHS = std::min(this->vaddr + elemSize, rangeVAddr + rangeSize);
  // Check that the overlap is within the same line.
  if (overlapRHS <= overlapLHS) {
    // There is no overlap.
    elemOffset = elemSize;
    rangeOffset = rangeSize;
    return 0;
  }
  assert(ruby::makeLineAddress(overlapLHS) ==
             ruby::makeLineAddress(overlapRHS - 1) &&
         "Illegal overlap.");
  auto overlapSize = overlapRHS - overlapLHS;
  rangeOffset = overlapLHS - rangeVAddr;
  elemOffset = overlapLHS - this->vaddr;
  return overlapSize;
}

void LLCStreamElement::extractElementDataFromSlice(
    GemForgeCPUDelegator *cpuDelegator, const DynStreamSliceId &sliceId,
    const ruby::DataBlock &dataBlock) {
  /**
   * Extract the element data and update the LLCStreamElement.
   */
  auto elemIdx = this->idx;
  auto elemSize = this->size;
  if (this->S->isMemStream()) {
    if (this->vaddr == 0) {
      LLC_ELEMENT_PANIC(this, "Cannot extract data without vaddr.");
    }
  } else {
    assert(this->vaddr == 0 && "Non-Mem Stream with Non-Zero VAddr.");
    assert(sliceId.vaddr == 0 && "Non-Mem Stream with Slice VAddr.");
  }

  int sliceOffset;
  int elemOffset;
  int overlapSize = this->computeOverlap(sliceId.vaddr, sliceId.getSize(),
                                         sliceOffset, elemOffset);
  assert(overlapSize > 0 && "Empty overlap.");
  if (!this->S->isMemStream()) {
    assert(overlapSize == elemSize && "Non-Mem Stream with Multi-Slice Elem.");
  }
  Addr overlapLHS = this->vaddr + elemOffset;

  LLC_SLICE_DPRINTF(sliceId,
                    "Recv elem %lu size %d [%lu, %lu) slice [%lu, %lu).\n",
                    elemIdx, elemSize, elemOffset, elemOffset + overlapSize,
                    sliceOffset, sliceOffset + overlapSize);

  // Get the data from the cache line.
  auto data = dataBlock.getData(
      overlapLHS % ruby::RubySystem::getBlockSizeBytes(), overlapSize);
  memcpy(this->getUInt8Ptr(elemOffset), data, overlapSize);
  if (Debug::LLCRubyStreamBase) {
    LLC_SLICE_DPRINTF(sliceId, "Extract elem data %dB %s.\n", overlapSize,
                      GemForgeUtils::dataToString(data, overlapSize));
  }

  // Mark these bytes ready.
  this->addReadyBytes(overlapSize);
  if (this->readyBytes > this->size) {
    LLC_SLICE_PANIC(
        sliceId,
        "Too many ready bytes %lu Overlap [%lu, %lu), ready %d > size %d.",
        elemIdx, elemOffset, elemOffset + overlapSize, this->readyBytes,
        this->size);
  }
  if (this->isReady()) {
    this->valueReadyCycle = this->mlcController->curCycle();
    LLC_ELEMENT_DPRINTF(this, "Elem Value Ready Cycle: Issue %lu Ready %lu.\n",
                        this->issueCycle, this->valueReadyCycle);
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
} // namespace gem5
