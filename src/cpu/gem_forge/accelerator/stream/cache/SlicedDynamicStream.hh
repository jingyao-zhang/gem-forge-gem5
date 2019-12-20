#ifndef __GEM_FORGE_SLICED_DYNAMIC_STREAM_H__
#define __GEM_FORGE_SLICED_DYNAMIC_STREAM_H__

/**
 * Since the cache stream engines manage streams in slice granularity,
 * we introduce this utility class to slice the stream.
 *
 * Only direct streams can be sliced.
 */

#include "CacheStreamConfigureData.hh"
#include "DynamicStreamSliceId.hh"

#include <deque>

class SlicedDynamicStream {
public:
  SlicedDynamicStream(CacheStreamConfigureData *_configData);

  DynamicStreamSliceId getNextSlice();
  const DynamicStreamSliceId &peekNextSlice() const;

private:
  DynamicStreamId streamId;
  DynamicStreamFormalParamV formalParams;
  AddrGenCallbackPtr addrGenCallback;
  int32_t elementSize;

  /**
   * Internal states.
   * ! Evil trick to make peekNextSlice constant.
   */
  mutable uint64_t tailIdx;
  /**
   * The headIdx that can be checked for slicing.
   */
  mutable uint64_t sliceHeadIdx;
  mutable std::deque<DynamicStreamSliceId> slices;

  void allocateOneElement() const;
};

#endif