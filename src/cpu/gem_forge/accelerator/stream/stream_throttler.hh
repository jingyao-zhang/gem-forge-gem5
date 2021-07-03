#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_THROTTLER_H__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_THROTTLER_H__

#include <string>

#include "stream_engine.hh"

/**
 * Helper class to throttle the stream's maxSize.
 */
class StreamThrottler {
public:
  enum StrategyE {
    STATIC,
    DYNAMIC,
    GLOBAL,
  };
  StrategyE strategy;
  StreamEngine *se;
  StreamThrottler(const std::string &_strategy, StreamEngine *_se);

  void throttleStream(StreamElement *element);
  const std::string name() const;

  /**
   * Try to manually boost allocation for certain streams.
   * Used for InnerMostLoop streams.
   */
  void boostStreams(const Stream::StreamVec &stepRootStreams);

private:
  bool tryGlobalThrottle(Stream *S);
};

#endif