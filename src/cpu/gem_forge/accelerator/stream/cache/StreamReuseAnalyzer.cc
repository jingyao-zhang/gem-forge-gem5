#include "StreamReuseAnalyzer.hh"

#include "../stream_float_policy.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStrandSplit.hh"

#define DEBUG_TYPE MLCRubyStrandSplit
#include "../stream_log.hh"

#define STRAND_LOG_(X, dynId, format, args...)                                 \
  {                                                                            \
    DYN_S_DPRINTF_(X, dynId, format, ##args);                                  \
    std::ostringstream s;                                                      \
    ccprintf(s, format, ##args);                                               \
    StreamFloatPolicy::logS(dynId) << s.str() << std::flush;                   \
  }

#define MLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(MLCRubyStrandSplit, "%s: " format, this->myName, ##args)

namespace gem5 {

StreamReuseInfo StreamReuseAnalyzer::analyzeReuse(ConfigPtr strand) const {

  StreamReuseInfo info;
  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(strand->addrGenCallback);
  if (!linearAddrGen) {
    return info;
  }
  if (strand->addrGenFormalParams.size() % 2 != 1) {
    // Missing final trip.
    return info;
  }

  auto reuseDim = linearAddrGen->getFirstReuseDim(strand->addrGenFormalParams);
  if (reuseDim < 0) {
    return info;
  }

  std::vector<int64_t> strides;
  std::vector<int64_t> trips;

  extractStrideAndTripFromAffinePatternParams(strand->addrGenFormalParams,
                                              strides, trips);
  assert(strides.at(reuseDim) == 0);

  auto reuseDimEnd = reuseDim + 1;
  while (reuseDimEnd < strides.size() &&
         (strides.at(reuseDimEnd) == 0 || trips.at(reuseDimEnd) == 1)) {
    reuseDimEnd++;
  }

  auto reuseCount = AffinePattern::reduce_mul(trips.begin() + reuseDim,
                                              trips.begin() + reuseDimEnd, 1);
  auto reuseTileSize =
      AffinePattern::reduce_mul(trips.begin(), trips.begin() + reuseDim, 1);

  info = StreamReuseInfo(reuseDim, reuseDimEnd, reuseCount, reuseTileSize);
  return info;
}
} // namespace gem5
