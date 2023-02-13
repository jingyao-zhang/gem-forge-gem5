#ifndef __CPU_GEM_FORGE_MLC_PREFETCH_MANAGER_HH__
#define __CPU_GEM_FORGE_MLC_PREFETCH_MANAGER_HH__

/**
 * Used to prefetch stream regions when the area is not cached.
 * Used by PUM and near-stream computing.
 */

#include "../MLCStreamEngine.hh"

namespace gem5 {

class MLCPrefetchManager {
public:
  MLCPrefetchManager(MLCStreamEngine *_mlcSE);

private:
  using ConfigPtr = CacheStreamConfigureDataPtr;
  MLCStreamEngine *mlcSE;
  ruby::AbstractStreamAwareController *controller;
};

} // namespace gem5

#endif