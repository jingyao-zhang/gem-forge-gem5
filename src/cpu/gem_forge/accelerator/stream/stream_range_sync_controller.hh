#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_RANGE_SYNC_CONTROLLER_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_RANGE_SYNC_CONTROLLER_HH__

#include "stream_engine.hh"

class StreamRangeSyncController {
public:
  StreamRangeSyncController(StreamEngine *_se);

  /**
   * Check that ranges for all active dynamic streams are ready,
   * so the core can start to commit and check against the range.
   * @return The first DynS that has no ready range.
   */
  DynamicStream *getNoRangeDynS();

  /**
   * Helper function to get the element idx we should check range for.
   */
  uint64_t getCheckElementIdx(DynamicStream *dynS);

private:
  StreamEngine *se;

  using DynStreamVec = std::vector<DynamicStream *>;

  DynStreamVec getCurrentDynStreams();
  void updateCurrentWorkingRange(DynStreamVec &dynStreams);
  void checkAliasBetweenRanges(DynStreamVec &dynStreams,
                               const DynamicStreamAddressRangePtr &newRange);
};

#endif