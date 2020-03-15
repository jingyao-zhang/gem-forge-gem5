#ifndef __GEM_FORGE_ACCELERATOR_STREAM_FLOAT_POLICY_HH__
#define __GEM_FORGE_ACCELERATOR_STREAM_FLOAT_POLICY_HH__

#include "stream.hh"

#include "base/output.hh"

class StreamFloatPolicy {
public:
  StreamFloatPolicy(bool _enabled, const std::string &_policy);
  ~StreamFloatPolicy();

  bool shouldFloatStream(Stream *S, DynamicStream &dynS);

private:
  bool enabled;
  enum PolicyE {
    STATIC,
    MANUAL,
    SMART,
  } policy;
  std::vector<uint64_t> privateCacheCapacity;

  bool shouldFloatStreamManual(Stream *S, DynamicStream &dynS);
  bool shouldFloatStreamSmart(Stream *S, DynamicStream &dynS);
  bool checkReuseWithinStream(Stream *S, DynamicStream &dynS);
  bool checkAggregateHistory(Stream *S, DynamicStream &dynS);

  static std::ostream &getLog() {
    assert(log && "No log for StreamFloatPolicy.");
    return *log->stream();
  }

  static std::ostream &logStream(Stream *S);

  static OutputStream *log;
};

#endif