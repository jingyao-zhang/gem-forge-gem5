#ifndef __GEM_FORGE_ACCELERATOR_STREAM_FLOAT_POLICY_HH__
#define __GEM_FORGE_ACCELERATOR_STREAM_FLOAT_POLICY_HH__

#include "stream.hh"

#include "base/output.hh"

class StreamFloatPolicy {
public:
  StreamFloatPolicy(bool _enabled, bool _enabledFloatMem,
                    const std::string &_policy,
                    const std::string &_levelPolicy);
  ~StreamFloatPolicy();

  struct FloatDecision {
    bool shouldFloat;
    MachineType floatMachineType;
    FloatDecision(
        bool _shouldFloat = false,
        MachineType _floatMachineType = MachineType::MachineType_L2Cache)
        : shouldFloat(_shouldFloat), floatMachineType(_floatMachineType) {}
  };

  FloatDecision shouldFloatStream(DynamicStream &dynS);
  MachineType chooseFloatMachineType(DynamicStream &dynS);

  bool shouldPseudoFloatStream(DynamicStream &dynS);

  static std::ostream &logStream(Stream *S);

private:
  bool enabled;
  bool enabledFloatMem;
  enum PolicyE {
    STATIC,
    MANUAL,
    SMART,
    SMART_COMPUTATION,
  } policy;
  enum LevelPolicyE {
    LEVEL_STATIC,
    LEVEL_MANUAL,
    LEVEL_SMART,
  } levelPolicy;
  std::vector<uint64_t> cacheCapacity;

  uint64_t getPrivateCacheCapacity() const {
    return this->cacheCapacity.at(1);
  }
  uint64_t getSharedLLCCapacity() const {
    return this->cacheCapacity.back();
  }

  FloatDecision shouldFloatStreamManual(DynamicStream &dynS);
  FloatDecision shouldFloatStreamSmart(DynamicStream &dynS);
  bool checkReuseWithinStream(DynamicStream &dynS);
  bool checkAggregateHistory(DynamicStream &dynS);

  static std::ostream &getLog() {
    assert(log && "No log for StreamFloatPolicy.");
    return *log->stream();
  }

  static OutputStream *log;
};

#endif