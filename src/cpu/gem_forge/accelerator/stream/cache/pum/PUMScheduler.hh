#ifndef __CPU_GEM_FORGE_PUM_SCHEDULER_HH__
#define __CPU_GEM_FORGE_PUM_SCHEDULER_HH__

#include "MLCPUMManager.hh"

/**
 * This class is in charge of scheduling and wordline allocation.
 */
class PUMScheduler {
public:
  PUMScheduler(MLCPUMManager *_manager);

  /**
   * Schedule PUMDataGraph nodes and insert sync nodes.
   * So far just BFS.
   */
  using PatternInfo = MLCPUMManager::PatternInfo;
  using PUMComputeStreamGroup = MLCPUMManager::PUMComputeStreamGroup;
  using PUMContext = MLCPUMManager::PUMContext;
  using PUMDataGraphNode = MLCPUMManager::PUMDataGraphNode;
  using PUMDataGraphNodeVec = MLCPUMManager::PUMDataGraphNodeVec;

  PUMDataGraphNodeVec schedulePUMDataGraph(PUMContext &context);

private:
  using ConfigPtr = CacheStreamConfigureDataPtr;
  MLCStreamEngine *mlcSE;
  AbstractStreamAwareController *controller;
  MLCPUMManager *manager;

  PUMDataGraphNodeVec schedulePUMDataGraphLinear(PUMContext &context);
  PUMDataGraphNodeVec schedulePUMDataGraphBFS(PUMContext &context,
                                              bool addSync);
  PUMDataGraphNodeVec schedulePUMDataGraphUnison(PUMContext &context);

  PUMDataGraphNodeVec insertSyncNodes(PUMContext &context,
                                      const PUMDataGraphNodeVec &nodes);
  void appendSyncNode(PUMContext &context, PUMDataGraphNodeVec &nodes);
};

#endif