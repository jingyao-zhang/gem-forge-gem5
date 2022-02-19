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
    FloatDecision(bool _shouldFloat = false) : shouldFloat(_shouldFloat) {}
  };

  FloatDecision shouldFloatStream(DynStream &dynS);

  bool shouldPseudoFloatStream(DynStream &dynS);

  static std::ostream &logS(const DynStream &dynS);

  /**
   * Set the float level for all streams.
   */
  using DynStreamList = std::list<DynStream *>;
  using StreamCacheConfigMap =
      std::unordered_map<Stream *, CacheStreamConfigureDataPtr>;
  void setFloatPlans(DynStreamList &dynStreams,
                     StreamCacheConfigMap &floatedMap,
                     CacheStreamConfigureVec &rootConfigVec);

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
    LEVEL_MANUAL2,
    LEVEL_SMART,
  } levelPolicy;
  std::vector<uint64_t> cacheCapacity;

  uint64_t getPrivateCacheCapacity() const { return this->cacheCapacity.at(1); }
  uint64_t getSharedLLCCapacity() const { return this->cacheCapacity.back(); }

  FloatDecision shouldFloatStreamManual(DynStream &dynS);
  FloatDecision shouldFloatStreamSmart(DynStream &dynS);
  bool checkReuseWithinStream(DynStream &dynS);
  bool checkAggregateHistory(DynStream &dynS);

  static std::ostream &getLog() {
    assert(log && "No log for StreamFloatPolicy.");
    return *log->stream();
  }

  static OutputStream *log;

  void setFloatPlan(DynStream &dynS);
  void setFloatPlanManual(DynStream &dynS);

  void setFloatPlanForRodiniaSrad(DynStream &dynS);
  void setFloatPlanForBinTree(DynStream &dynS);

  static const std::unordered_map<std::string, std::string> streamToRegionMap;
};

#endif