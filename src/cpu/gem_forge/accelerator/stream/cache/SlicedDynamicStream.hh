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
  const DynamicStreamSliceId &peekNextSlice();

private:
  DynamicStreamId streamId;
  DynamicStreamFormalParamV formalParams;
  AddrGenCallbackPtr addrGenCallback;
  int32_t elementSize;

  /**
   * Internal states.
   */
  uint64_t tailIdx;
  std::deque<DynamicStreamSliceId> slices;

  void allocateOneElement();
};

#endif