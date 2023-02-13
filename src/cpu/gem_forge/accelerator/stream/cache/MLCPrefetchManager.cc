#include "MLCPrefetchManager.hh"

namespace gem5 {

MLCPrefetchManager::MLCPrefetchManager(MLCStreamEngine *_mlcSE)
    : mlcSE(_mlcSE), controller(_mlcSE->controller) {}

} // namespace gem5

