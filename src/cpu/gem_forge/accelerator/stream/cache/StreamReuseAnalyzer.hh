#ifndef GEM_FORGE_STREAM_REUSE_ANALYZER_HH
#define GEM_FORGE_STREAM_REUSE_ANALYZER_HH

#include "CacheStreamConfigureData.hh"

namespace gem5 {

class StreamReuseAnalyzer {

public:
  using Config = CacheStreamConfigureData;
  using ConfigPtr = CacheStreamConfigureDataPtr;

  StreamReuseAnalyzer(std::string _myName) : myName(_myName) {}

  StreamReuseInfo analyzeReuse(ConfigPtr strand) const;

private:
  std::string myName;
};

} // namespace gem5

#endif