#include "PUMScheduler.hh"

PUMScheduler::PUMScheduler(MLCPUMManager *_manager)
    : mlcSE(_manager->mlcSE), controller(_manager->controller),
      manager(_manager) {}

PUMScheduler::PUMDataGraphNodeVec
PUMScheduler::schedulePUMDataGraph(PUMContext &context) {
  if (this->controller->myParams->stream_pum_schedule_type == "bfs") {
    return this->schedulePUMDataGraphBFS(context);
  } else if (this->controller->myParams->stream_pum_schedule_type == "unison") {
    return this->schedulePUMDataGraphUnison(context);
  } else if (this->controller->myParams->stream_pum_schedule_type == "linear") {
    return this->schedulePUMDataGraphLinear(context);
  } else {
    panic("Unknown PUM TDFG scheduler.");
  }
}

PUMScheduler::PUMDataGraphNodeVec
PUMScheduler::schedulePUMDataGraphLinear(PUMContext &context) {

  /**
   * This is used when we don't optimize the DFG. Here we simply
   * insert Sync node before and after CMP node.
   */

  PUMDataGraphNodeVec scheduledNodes;

  for (auto node : context.pumDataGraphNodes) {
    if (node->type != PUMDataGraphNode::TypeE::Compute) {
      scheduledNodes.push_back(node);
      continue;
    }
    // Check if the previous node is Sync.
    if (!scheduledNodes.empty() &&
        scheduledNodes.back()->type != PUMDataGraphNode::TypeE::Sync) {
      scheduledNodes.push_back(PUMDataGraphNode::newSyncNode());
    }
    scheduledNodes.push_back(node);
    // Insert a sync after that.
    scheduledNodes.push_back(PUMDataGraphNode::newSyncNode());
  }

  return scheduledNodes;
}

PUMScheduler::PUMDataGraphNodeVec
PUMScheduler::schedulePUMDataGraphBFS(PUMContext &context) {

  PUMDataGraphNodeVec scheduledNodes;

  std::set<PUMDataGraphNode *> scheduled;
  std::set<PUMDataGraphNode *> frontier;
  for (auto node : context.pumDataGraphNodes) {
    if (node->operands.empty()) {
      frontier.insert(node);
      scheduled.insert(node);
      scheduledNodes.push_back(node);
    }
  }

  while (!frontier.empty()) {
    std::set<PUMDataGraphNode *> nextFrontier;
    for (auto node : frontier) {
      for (auto user : node->users) {
        bool allOperandsScheduled = true;
        for (auto operand : user->operands) {
          if (!scheduled.count(operand)) {
            allOperandsScheduled = false;
            break;
          }
        }
        if (allOperandsScheduled) {
          nextFrontier.insert(user);
        }
      }
    }
    /**
     * No need to sync if both frontiers contain only ComputeNodes.
     */
    bool needSync = true;
    {
      auto areAllCmpNodes =
          [](const std::set<PUMDataGraphNode *> &nodes) -> bool {
        for (auto node : nodes) {
          if (node->type != PUMDataGraphNode::TypeE::Compute) {
            return false;
          }
        }
        return true;
      };
      auto areAllValueOrConstNodes =
          [](const std::set<PUMDataGraphNode *> &nodes) -> bool {
        for (auto node : nodes) {
          if (node->type == PUMDataGraphNode::TypeE::Value) {
            // Value node.
            continue;
          }
          if (node->type == PUMDataGraphNode::TypeE::Compute &&
              node->compValTy != PUMDataGraphNode::CompValueE::None) {
            // Const value node.
            continue;
          }
          return false;
        }
        return true;
      };
      if (areAllCmpNodes(frontier) && areAllCmpNodes(nextFrontier)) {
        needSync = false;
      }
      if (areAllValueOrConstNodes(frontier)) {
        needSync = false;
      }
    }
    // Insert a sync node.
    if (needSync) {
      scheduledNodes.push_back(PUMDataGraphNode::newSyncNode());
    }
    for (auto node : nextFrontier) {
      scheduledNodes.push_back(node);
      scheduled.insert(node);
    }
    frontier = nextFrontier;
  }

  if (scheduledNodes.back()->type != PUMDataGraphNode::TypeE::Sync) {
    // Make sure we always sync at the end.
    scheduledNodes.push_back(PUMDataGraphNode::newSyncNode());
  }

  return scheduledNodes;
}

PUMScheduler::PUMDataGraphNodeVec
PUMScheduler::schedulePUMDataGraphUnison(PUMContext &context) {

  return this->schedulePUMDataGraphUnison(context);
}