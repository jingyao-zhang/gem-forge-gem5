#ifndef __CPU_GEM_FORGE_LLC_STREAM_SLICE_HH__
#define __CPU_GEM_FORGE_LLC_STREAM_SLICE_HH__

#include "DynamicStreamSliceId.hh"

#include <memory>

class LLCStreamSlice;
using LLCStreamSlicePtr = std::shared_ptr<LLCStreamSlice>;

/**
 * Each LLCDynamicStream is managed at two level of granularity:
 * LLCStreamElement: 
 *  This is the basic unit to interact with the core (one iteration).
 *  Thus, this is used for computing and range-sync.
 * LLCStreamSlice:
 *  This is the basic unit to interact with the cache (one request).
 * 
 * There exists a mapping relationship between these two units:
 * 1. For direct streams, this is a multi-to-multi mapping, subjected
 * to coalescing continuous elements requesting the same cache line,
 * and multi-line elements.
 * 2. For indirect streams, this is one-to-many mapping, as one element
 * can still access at most two lines, but we don't coalesce indirect
 * elements to the same cache line.
 * 
 * There are some exceptions:
 * 1. If an element is a placeholder to receive data from a stream in
 * other bank, it has no corresponding slice as it does not issue to
 * memory.
 * 2. ReductionStream has no slices, of course.
 * 
 * The element remembers a vector of pointers, but not the other way
 * around to break circular dependence.
 */

class LLCStreamSlice {
public:
  LLCStreamSlice(const DynamicStreamSliceId &_sliceId);

  enum State {
    INITIALIZED,
    ALLOCATED,
    ISSUED,
    RESPONDED,
  };
  State getState() const { return this->state; }
  void setState(State state) { this->state = state; }

  const DynamicStreamSliceId &getSliceId() const {
    return this->sliceId;
  }

private:
  DynamicStreamSliceId sliceId;
  State state = State::INITIALIZED;
};

#endif