#ifndef __GEM_FORGE_SLICED_DYN_STREAM_H__
#define __GEM_FORGE_SLICED_DYN_STREAM_H__

/**
 * This will slice the stream into slices. A slice is a piece of the stream,
 * and can span across multiple elements (when continuous elements are
 * coalesced) or be sub-element (when multiple streams are coalesced into a
 * wider stream).
 */

#include "CacheStreamConfigureData.hh"
#include "DynStreamSliceId.hh"

#include <deque>

class SlicedDynStream {
public:
  SlicedDynStream(CacheStreamConfigureDataPtr _configData);

  DynStreamSliceId getNextSlice();
  const DynStreamSliceId &peekNextSlice() const;

  /**
   * Check if we have allocated beyond the end of the stream.
   * Instead of terminating the stream, here I take a "soft"
   * approach to ease the implementation complexicity.
   *
   * Notice that we allow (totalTripCount + 1) elements as
   * StreamEnd will consume one element and we have to be synchronized
   * with the core's StreamEngine.
   */
  bool hasOverflowed() const {
    return this->hasOverflowed(this->peekNextSlice().getStartIdx());
  }

  /**
   * To support StreamLoopBound, all offloaded streams should eventually
   * query this for the latest TotalTripCount.
   */
  int64_t getTotalTripCount() const { return this->totalTripCount; }
  bool hasTotalTripCount() const {
    return this->totalTripCount != InvalidTripCount;
  }
  int64_t getInnerTripCount() const { return this->innerTripCount; }
  bool hasInnerTripCount() const {
    return this->innerTripCount != InvalidTripCount;
  }
  void setTotalAndInnerTripCount(int64_t tripCount);

  /**
   * Helper function to get element vaddr and size.
   */
  Addr getElementVAddr(uint64_t elementIdx) const;
  int32_t getMemElementSize() const { return this->elemSize; }
  float getElementPerSlice() const { return this->elemPerSlice; }

private:
  DynStrandId strandId;
  DynStreamFormalParamV formalParams;
  AddrGenCallbackPtr addrGenCallback;
  const int64_t stepElemCount;
  const int32_t elemSize;
  // On average how many elements per slice.
  float elemPerSlice = 1.0f;
  /**
   * -1 means indefinite.
   */
  static constexpr int64_t InvalidTripCount =
      CacheStreamConfigureData::InvalidTripCount;
  int64_t totalTripCount = InvalidTripCount;
  int64_t innerTripCount = InvalidTripCount;

  /**
   * Whether we could coalesce continuous elements into slices.
   * Only false for PointerChaseStream.
   */
  bool coalesceContinuousElements = true;

  /**
   * ! So far PtrChaseStream is handled by an oracle
   * ! read to compute the address.
   */
  bool isPtChase = false;

  struct PtrChaseState {
    Stream *memStream = nullptr;
    Stream *ivStream = nullptr;
    DynStreamFormalParamV ivAddrFormalParams;
    AddrGenCallbackPtr ivAddrGenCallback;
    StreamValue currentIVValue;
    bool currentIVValueFaulted = false;
    // Buffer all element virtual addresses.
    std::vector<Addr> elementVAddrs;
    PtrChaseState(CacheStreamConfigureDataPtr &_configData);
  };
  mutable PtrChaseState ptrChaseState;
  Addr getOrComputePtrChaseElemVAddr(uint64_t elemIdx) const;

  /**
   * Internal states.
   * ! Evil trick to make peekNextSlice constant.
   */
  mutable uint64_t prevTailElemIdx;
  mutable uint64_t tailElemIdx;
  void stepTailElemIdx() const;

  /**
   * The headIdx that can be checked for slicing.
   */
  mutable uint64_t sliceHeadElemIdx;
  mutable std::deque<DynStreamSliceId> slices;

  void allocateOneElement() const;
  bool hasOverflowed(uint64_t elemIdx) const {
    return this->hasTotalTripCount() && elemIdx >= (this->totalTripCount);
  }
};

#endif
