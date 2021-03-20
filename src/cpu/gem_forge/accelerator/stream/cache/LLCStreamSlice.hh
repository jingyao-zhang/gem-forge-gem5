#ifndef __CPU_GEM_FORGE_LLC_STREAM_SLICE_HH__
#define __CPU_GEM_FORGE_LLC_STREAM_SLICE_HH__

#include "DynamicStreamSliceId.hh"

#include "mem/ruby/common/DataBlock.hh"

#include <memory>

class LLCStreamSlice;
using LLCStreamSlicePtr = std::shared_ptr<LLCStreamSlice>;

class LLCStreamEngine;

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
    /**
     * The states are:
     * 1. INITIALIZED: Initialized in the MLC SE. Can not be used yet in LLC.
     * 2. ALLOCATED: The LLC SE received the credit and allocated it.
     * 3. ISSUED: The LLC SE issued the request to the cache.
     * 4. RESPONEDED: The LLC SE already received the response.
     * 5. FAULTED: The slice has faulted virtual address.
     * 6. RELEASED: The slice is released by LLC SE.
     * Some tricky points:
     * 1. For indirect requests, it is the remote LLC SE who received the
     * response.
     */
    INITIALIZED,
    ALLOCATED,
    ISSUED,
    RESPONDED,
    FAULTED,
    RELEASED,
  };
  State getState() const { return this->state; }

  void allocate(LLCStreamEngine *llcSE);
  void issue();
  void responded(const DataBlock &loadBlock, const DataBlock &storeBlock);
  void faulted();
  void released();

  const DynamicStreamSliceId &getSliceId() const { return this->sliceId; }

  const DataBlock &getLoadBlock() const { return this->loadBlock; }
  const DataBlock &getStoreBlock() const { return this->storeBlock; }

  bool isLoadComputeValueSent() const { return this->loadComputeValueSent; }
  void setLoadComputeValueSent();

private:
  DynamicStreamSliceId sliceId;
  State state = State::INITIALIZED;
  LLCStreamEngine *llcSE = nullptr;
  DataBlock loadBlock;
  DataBlock storeBlock;

  /**
   * Whether the LoadComputeValue has been sent to the core.
   */
  bool loadComputeValueSent = false;
};

#endif