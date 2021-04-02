#ifndef __CPU_TDG_ACCELERATOR_LLC_STREAM_ELEMENT_H__
#define __CPU_TDG_ACCELERATOR_LLC_STREAM_ELEMENT_H__

#include "LLCStreamSlice.hh"

#include "cpu/gem_forge/accelerator/stream/stream.hh"
#include "mem/ruby/common/DataBlock.hh"
#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include <memory>

struct LLCStreamElement;
using LLCStreamElementPtr = std::shared_ptr<LLCStreamElement>;
using ConstLLCStreamElementPtr = std::shared_ptr<const LLCStreamElement>;

class LLCStreamElement {
public:
  /**
   * This represents the basic unit of LLCStreamElement.
   * It remembers the base elements it depends on. Since this can be a
   * remote LLCDynamicStream sending here, we do not remember LLCDynamicStream
   * in the element, but just the DynamicStreamId and the StaticStream.
   */
  LLCStreamElement(Stream *_S, AbstractStreamAwareController *_mlcController,
                   const DynamicStreamId &_dynStreamId, uint64_t _idx,
                   Addr _vaddr, int _size);

  Stream *S;
  AbstractStreamAwareController *mlcController;
  const DynamicStreamId dynStreamId;
  const uint64_t idx;
  const int size;
  Addr vaddr = 0;

  int curLLCBank() const;

  std::vector<LLCStreamElementPtr> baseElements;
  bool areBaseElementsReady() const {
    for (const auto &baseElement : this->baseElements) {
      if (!baseElement->isReady()) {
        return false;
      }
    }
    return true;
  }

  bool areSlicesReleased() const {
    for (int i = 0; i < this->numSlices; ++i) {
      const auto &slice = this->slices.at(i);
      if (slice->getState() != LLCStreamSlice::State::RELEASED) {
        return false;
      }
    }
    return true;
  }

  StreamValue getBaseStreamValue(uint64_t baseStreamId);
  StreamValue getBaseOrMyStreamValue(uint64_t streamId);

  /*************************************************
   * Accessors to the data.
   *************************************************/
  bool isReady() const { return this->readyBytes == this->size; }
  bool isComputationScheduled() const { return this->computationScheduled; }
  void scheduledComputation() {
    assert(!this->computationScheduled && "Computation already scheduled.");
    this->computationScheduled = true;
  }
  bool isComputationDone() const { return this->computationDone; }
  void doneComputation() {
    assert(this->isComputationScheduled() &&
           "Computation done before scheduled.");
    assert(!this->computationDone && "Computaion already done.");
    this->computationDone = true;
  }

  StreamValue getValue(int offset = 0, int size = sizeof(StreamValue)) const;
  uint8_t *getUInt8Ptr(int offset = 0);
  const uint8_t *getUInt8Ptr(int offset = 0) const;
  uint64_t getUInt64() const {
    assert(this->isReady());
    assert(this->size <= sizeof(uint64_t));
    return this->value.front();
  }
  StreamValue getValueByStreamId(uint64_t streamId) const;
  uint64_t getUInt64ByStreamId(uint64_t streamId) const;

  void setValue(const StreamValue &value);

  void extractElementDataFromSlice(GemForgeCPUDelegator *cpuDelegator,
                                   const DynamicStreamSliceId &sliceId,
                                   const DataBlock &dataBlock);

  /**
   * Helper function to compute the overlap between the a range and the element.
   * @return: the size of the overlap.
   */
  int computeOverlap(Addr rangeVAddr, int rangeSize, int &rangeOffset,
                     int &elementOffset) const;

  void addSlice(LLCStreamSlicePtr &slice);
  int getNumSlices() const { return this->numSlices; }

  /**
   * Remember the state of the element. This is so far just used for
   * IndirectStream.
   */
  enum State {
    INITIALIZED,
    READY_TO_ISSUE,
    ISSUED,
  };

  State getState() const { return this->state; }
  void setState(State state) { this->state = state; }

  void setCoreCommitted() { this->coreCommitted = true; }
  bool hasCoreCommitted() const { return this->coreCommitted; }

  void setIndirectTriggered() { this->indirectTriggered = true; }
  bool hasIndirectTriggered() const { return this->indirectTriggered; }

  void setRangeBuilt() { this->rangeBuilt = true; }
  bool hasRangeBuilt() const { return this->rangeBuilt; }

  void setFirstIndirectAtomicReqSeen() {
    this->firstIndirectAtomicReqSeen = true;
  }
  bool hasFirstIndirectAtomicReqSeen() const {
    return this->firstIndirectAtomicReqSeen;
  }
  void setSecondIndirectAtomicReqSeen() {
    assert(this->hasFirstIndirectAtomicReqSeen() &&
           "Second before first IndirectAtomicRequest.");
    assert(!this->hasSecondIndirectAtomicReqSeen() &&
           "Second IndirectAtomicRequest already seen.");
    this->secondIndirectAtomicReqSeen = true;
  }
  bool hasSecondIndirectAtomicReqSeen() const {
    return this->secondIndirectAtomicReqSeen;
  }

  bool isIndirectStoreAcked() const { return this->indirectStoreAcked; }
  void setIndirectStoreAcked() {
    assert(!this->indirectStoreAcked && "IndirectStore already acked.");
    this->indirectStoreAcked = true;
  }

  bool isComputedValueReady() const { return this->computedValueReady; }
  const StreamValue &getComputedValue() const { return this->computedValue; }
  void setComputedValue(const StreamValue &value);

  const LLCStreamElementPtr &getPrevReductionElement() const {
    return this->prevReductionElement;
  }
  void setPrevReductionElement(const LLCStreamElementPtr &element) {
    this->prevReductionElement = element;
  }

private:
  int readyBytes;
  bool computationScheduled = false;
  bool computationDone = false;
  static constexpr int MAX_SIZE = 128;
  std::array<uint64_t, MAX_SIZE> value;

  static constexpr int MAX_SLICES_PER_ELEMENT = 2;
  std::array<LLCStreamSlicePtr, MAX_SLICES_PER_ELEMENT> slices;
  int numSlices = 0;

  State state = State::INITIALIZED;

  /**
   * We have received the StreamCommit from the core.
   */
  bool coreCommitted = false;
  /**
   * We have triggered the indirect elements.
   */
  bool indirectTriggered = false;
  /**
   * We have added this element to the RangeBuilder.
   * This is used to prevent multi-line elements from being added multiple
   * times.
   */
  bool rangeBuilt = false;
  /**
   * Whether we have seen the 1st/2nd request for this IndirectAtomicRequest.
   */
  bool firstIndirectAtomicReqSeen = false;
  bool secondIndirectAtomicReqSeen = false;

  /**
   * IndirectStoreAcked.
   */
  bool indirectStoreAcked = false;

  /**
   * Computation Result. Currently used for:
   * 1. LoadComputeStream.
   * 2. UpdateStream's StoreValue.
   */
  StreamValue computedValue;
  bool computedValueReady = false;

  /**
   * Explicitly remember the previous element for ReductionStream.
   */
  LLCStreamElementPtr prevReductionElement = nullptr;
};

#endif