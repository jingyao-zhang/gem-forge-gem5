#ifndef __GEM_FORGE_ACCELERATOR_STREAM_FLOAT_POLICY_HH__
#define __GEM_FORGE_ACCELERATOR_STREAM_FLOAT_POLICY_HH__

#include "stream.hh"

class StreamFloatPolicy {
public:
  StreamFloatPolicy(bool _enabled, const std::string &_policy);
  bool shouldFloatStream(Stream *S, uint64_t streamInstance);

private:
  bool enabled;
  enum PolicyE {
    STATIC,
    MANUAL,
  } policy;

  bool shouldFloatStreamManual(Stream *S, uint64_t streamInstance);
};

#endif