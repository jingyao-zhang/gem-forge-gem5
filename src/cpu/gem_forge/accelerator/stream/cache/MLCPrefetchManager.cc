#include "MLCPrefetchManager.hh"

MLCPrefetchManager::MLCPrefetchManager(MLCStreamEngine *_mlcSE)
    : mlcSE(_mlcSE), controller(_mlcSE->controller) {}

