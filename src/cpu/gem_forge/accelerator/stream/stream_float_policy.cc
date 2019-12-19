#include "stream_float_policy.hh"

#include "stream_engine.hh"

StreamFloatPolicy::StreamFloatPolicy(bool _enabled, const std::string &_policy)
    : enabled(_enabled) {
  if (_policy == "static") {
    this->policy = PolicyE::STATIC;
  } else if (_policy == "manual") {
    this->policy = PolicyE::MANUAL;
  } else {
    panic("Invalid StreamFloatPolicy.");
  }
}

bool StreamFloatPolicy::shouldFloatStream(Stream *S, uint64_t streamInstance) {
  if (!this->enabled) {
    return false;
  }
  if (!S->isDirectLoadStream() && !S->isPointerChaseLoadStream()) {
    return false;
  }
  /**
   * Make sure we do not offload empty stream.
   * This information may be known at configuration time, or even require
   * oracle information. However, as the stream is empty, trace-based
   * simulation does not know which LLC bank should the stream be offloaded
   * to.
   * TODO: Improve this.
   */
  if (S->se->isTraceSim()) {
    if (S->getStreamLengthAtInstance(streamInstance) == 0) {
      return false;
    }
  }

  switch (this->policy) {
  case PolicyE::STATIC:
    return true;
  case PolicyE::MANUAL: {
    return this->shouldFloatStreamManual(S, streamInstance);
  }
  default: { return false; }
  }

  // Let's use the previous staistic of the average stream.
  bool enableSmartDecision = false;
  if (enableSmartDecision) {
    const auto &statistic = S->statistic;
    if (statistic.numConfigured == 0) {
      // First time, maybe we aggressively offload as this is the
      // case for many microbenchmark we designed.
      return true;
    }
    auto avgLength = statistic.numUsed / statistic.numConfigured;
    if (avgLength < 500) {
      return false;
    }
  }
}

bool StreamFloatPolicy::shouldFloatStreamManual(Stream *S,
                                                uint64_t streamInstance) {
  /**
   * ! Hacky giant if/else for manually selection.
   * TODO: Really should be a hint in the stream configuration provided by the
   * compiler.
   */
  static std::unordered_map<Stream *, bool> memorizedDecision;
  auto iter = memorizedDecision.find(S);
  if (iter == memorizedDecision.end()) {
    auto shouldFloat = S->getFloatManual();
    iter = memorizedDecision.emplace(S, shouldFloat).first;
  }

  return iter->second;
}