#ifndef __GEM_FORGE_ACCELERATOR_STREAM_FLOAT_POLICY_HH__
#define __GEM_FORGE_ACCELERATOR_STREAM_FLOAT_POLICY_HH__

#include "stream.hh"

#include "base/output.hh"

class StreamFloatPolicy {
public:
  StreamFloatPolicy(bool _enabled, const std::string &_policy);
  ~StreamFloatPolicy();

  bool shouldFloatStream(DynamicStream &dynS);
  bool shouldPseudoFloatStream(DynamicStream &dynS);

private:
  bool enabled;
  enum PolicyE {
    STATIC,
    MANUAL,
    SMART,
    SMART_COMPUTATION,
  } policy;
  std::vector<uint64_t> privateCacheCapacity;

  bool shouldFloatStreamManual(DynamicStream &dynS);
  bool shouldFloatStreamSmart(DynamicStream &dynS);
  bool checkReuseWithinStream(DynamicStream &dynS);
  bool checkAggregateHistory(DynamicStream &dynS);

  static std::ostream &getLog() {
    assert(log && "No log for StreamFloatPolicy.");
    return *log->stream();
  }

  static std::ostream &logStream(Stream *S);

  static OutputStream *log;
};

#endif