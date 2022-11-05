#include "PUMScheduler.hh"

#include "../../stream_float_policy.hh"

#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "debug/MLCStreamPUM.hh"

#define DEBUG_TYPE MLCStreamPUM
#include "../../stream_log.hh"

#define MLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(MLCStreamPUM, "[MLC_SE%d]: [PUM] " format,                           \
          this->controller->getMachineID().num, ##args)
#define MLCSE_PANIC(format, args...)                                           \
  panic("[MLC_SE%d]: [PUM] " format, this->controller->getMachineID().num,     \
        ##args)

#define PUM_LOG_(format, args...)                                              \
  {                                                                            \
    MLCSE_DPRINTF(format, ##args);                                             \
    std::ostringstream __s;                                                    \
    ccprintf(__s, format, ##args);                                             \
    StreamFloatPolicy::getLog() << __s.str() << std::flush;                    \
  }

std::string exec(const char *cmd) {
  std::array<char, 128> buffer;
  std::string result;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
  if (!pipe) {
    panic("popen() failed!");
  }
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    inform("%s", buffer.data());
    result += buffer.data();
  }
  return result;
}

PUMScheduler::PUMScheduler(MLCPUMManager *_manager)
    : mlcSE(_manager->mlcSE), controller(_manager->controller),
      manager(_manager) {}

PUMScheduler::PUMDataGraphNodeVec
PUMScheduler::schedulePUMDataGraph(PUMContext &context) {
  if (this->controller->myParams->stream_pum_schedule_type == "bfs") {
    return this->schedulePUMDataGraphBFS(context, true /* addSync */);
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
      appendSyncNode(context, scheduledNodes);
    }
    scheduledNodes.push_back(node);
    // Insert a sync after that.
    appendSyncNode(context, scheduledNodes);
  }

  return scheduledNodes;
}

PUMScheduler::PUMDataGraphNodeVec
PUMScheduler::schedulePUMDataGraphBFS(PUMContext &context, bool addSync) {

  PUMDataGraphNodeVec scheduledNodes;

  std::set<PUMDataGraphNode *> scheduled;
  PUMDataGraphNodeVec frontier;
  for (auto node : context.pumDataGraphNodes) {
    if (node->operands.empty()) {
      frontier.push_back(node);
      scheduled.insert(node);
      scheduledNodes.push_back(node);
    }
  }

  while (!frontier.empty()) {
    // Use vector to get a fixed order?
    PUMDataGraphNodeVec nextFrontier;
    for (auto node : frontier) {
      for (auto user : node->users) {
        bool allOperandsScheduled = true;
        for (auto operand : user->operands) {
          if (!scheduled.count(operand)) {
            allOperandsScheduled = false;
            break;
          }
        }
        if (allOperandsScheduled &&
            !std::count(nextFrontier.begin(), nextFrontier.end(), user)) {
          nextFrontier.push_back(user);
        }
      }
    }
    /**
     * No need to sync if both frontiers contain only ComputeNodes.
     */
    bool needSync = true;
    {
      auto areAllCmpNodes = [](const PUMDataGraphNodeVec &nodes) -> bool {
        for (auto node : nodes) {
          if (node->type != PUMDataGraphNode::TypeE::Compute) {
            return false;
          }
        }
        return true;
      };
      auto areAllValueOrConstNodes =
          [](const PUMDataGraphNodeVec &nodes) -> bool {
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
    if (needSync && addSync) {
      appendSyncNode(context, scheduledNodes);
    }
    for (auto node : nextFrontier) {
      scheduledNodes.push_back(node);
      scheduled.insert(node);
    }
    frontier = nextFrontier;
  }

  if (scheduledNodes.back()->type != PUMDataGraphNode::TypeE::Sync && addSync) {
    // Make sure we always sync at the end.
    appendSyncNode(context, scheduledNodes);
  }

  return scheduledNodes;
}

void PUMScheduler::appendSyncNode(PUMContext &context,
                                  PUMDataGraphNodeVec &nodes) {
  nodes.push_back(PUMDataGraphNode::newSyncNode());
  context.pumDataGraphNodes.push_back(nodes.back());
}

PUMScheduler::PUMDataGraphNodeVec
PUMScheduler::insertSyncNodes(PUMContext &context,
                              const PUMDataGraphNodeVec &nodes) {

  PUMDataGraphNodeVec ret;

  /**
   * A state machine:
   * Init.
   * Move.
   * Cmp.
   * Ld.
   */
  const int Init = 0;
  const int Move = 1;
  const int Cmp = 2;
  const int Ld = 3;
  int state = Init;
  for (auto node : nodes) {
    if (node->type == PUMDataGraphNode::TypeE::Compute) {
      switch (state) {
      case Ld:
      case Move: {
        appendSyncNode(context, ret);
        break;
      }
      }
      state = Cmp;
    } else if (node->type == PUMDataGraphNode::TypeE::Move) {
      switch (state) {
      case Ld:
      case Cmp: {
        appendSyncNode(context, ret);
        break;
      }
      }
      state = Move;
    } else if (node->type == PUMDataGraphNode::TypeE::Value) {
      switch (state) {
      case Cmp: {
        appendSyncNode(context, ret);
        break;
      }
      }
      state = Ld;
    } else if (node->type == PUMDataGraphNode::TypeE::Load) {
      switch (state) {
      case Cmp: {
        appendSyncNode(context, ret);
        break;
      }
      }
      state = Ld;
    }

    ret.push_back(node);
  }

  if (ret.back()->type != PUMDataGraphNode::TypeE::Sync) {
    // Make sure we always sync at the end.
    appendSyncNode(context, ret);
  }

  return ret;
}

PUMScheduler::PUMDataGraphNodeVec
PUMScheduler::schedulePUMDataGraphUnison(PUMContext &context) {

  auto initScheduledNodes =
      this->schedulePUMDataGraphBFS(context, false /* addSync */);

  /**
   * Generate the MIR function.
   */
  std::ostringstream s;

  s << "--- |\n";
  s << " (corresponding IR)\n";
  s << "...\n";
  s << "---\n";
  s << "name: foo\n";
  s << "body: |\n";
  s << "  bb.0 (freq 20):\n";
  s << "    liveouts: %r2\n";

  std::map<MLCPUMManager::PUMDataGraphNode *, int> nodeToVRegMap;
  int usedVReg = 0;
  auto allocVReg = [&nodeToVRegMap,
                    &usedVReg](MLCPUMManager::PUMDataGraphNode *node) -> int {
    panic_if(nodeToVRegMap.count(node), "Realloc VReg for Node.");
    auto vReg = usedVReg++;
    nodeToVRegMap.emplace(node, vReg);
    return vReg;
  };
  auto getVReg =
      [&nodeToVRegMap](MLCPUMManager::PUMDataGraphNode *node) -> int {
    panic_if(!nodeToVRegMap.count(node), "No VReg for Node.");
    return nodeToVRegMap.at(node);
  };

  std::map<Addr, int> regionVAddrVRegMap;
  auto getOrAllocVRegion = [&regionVAddrVRegMap,
                            &usedVReg](Addr regionVAddr) -> int {
    if (regionVAddrVRegMap.count(regionVAddr)) {
      return regionVAddrVRegMap.at(regionVAddr);
    }
    auto vReg = usedVReg++;
    regionVAddrVRegMap.emplace(regionVAddr, vReg);
    return vReg;
  };

  std::map<int, MLCPUMManager::PUMDataGraphNode *> instIdToNodeMap;
  auto allocInstId =
      [&instIdToNodeMap](MLCPUMManager::PUMDataGraphNode *node) -> int {
    auto nextInstId = instIdToNodeMap.size();
    instIdToNodeMap.emplace(nextInstId, node);
    return nextInstId;
  };

  const int instBufSize = 1024;
  char instBuf[instBufSize];

  for (auto node : initScheduledNodes) {
    switch (node->type) {
    case MLCPUMManager::PUMDataGraphNode::TypeE::Value: {

      bool alreadyLoaded = regionVAddrVRegMap.count(node->regionVAddr);
      auto regionVReg = getOrAllocVRegion(node->regionVAddr);
      auto instId = allocInstId(node);
      if (!alreadyLoaded) {
        // There is the first time. Create inf_ld.
        snprintf(instBuf, instBufSize, "    %%%d = inf_ld %d\n", regionVReg,
                 instId);
        s << instBuf;
      }

      // Map the node to the inf_ld.
      nodeToVRegMap.emplace(node, regionVReg);
      break;
    }

    case MLCPUMManager::PUMDataGraphNode::TypeE::Sync: {
      // There should be no Sync in the initial scheduling.
      panic("Unsupported Node %s in Initial Scheduling.\n", *node);
    }

    case MLCPUMManager::PUMDataGraphNode::TypeE::Move: {
      // Move should have only one operand.
      assert(node->operands.size() == 1);
      auto src = node->operands.front();

      auto srcVReg = getVReg(src);
      auto vReg = allocVReg(node);
      auto instId = allocInstId(node);

      snprintf(instBuf, instBufSize, "    %%%d = inf_mv %d, %%%d\n", vReg,
               instId, srcVReg);
      s << instBuf;
      break;
    }

    case MLCPUMManager::PUMDataGraphNode::TypeE::Compute: {

      // Ignore the immediate value.
      if (node->isConstVal()) {
        break;
      }

      PUMDataGraphNodeVec srcs;
      const int maxNumSrcNodes = 3;
      for (auto operand : node->operands) {
        if (operand->isConstVal()) {
          continue;
        }
        srcs.push_back(operand);
        if (srcs.size() > maxNumSrcNodes) {
          panic("Unsupported # operands for %s.\n", *node);
        }
      }

      auto instId = allocInstId(node);

      if (srcs.size() == 3) {

        auto src1VReg = getVReg(srcs[0]);
        auto src2VReg = getVReg(srcs[1]);
        auto src3VReg = getVReg(srcs[2]);
        auto vReg = allocVReg(node);

        snprintf(instBuf, instBufSize,
                 "    %%%d = inf_cmp3 %d, %%%d, %%%d, %%%d\n", vReg, instId,
                 src1VReg, src2VReg, src3VReg);
      } else if (srcs.size() == 2) {

        auto src1VReg = getVReg(srcs[0]);
        auto src2VReg = getVReg(srcs[1]);
        auto vReg = allocVReg(node);

        snprintf(instBuf, instBufSize, "    %%%d = inf_cmp2 %d, %%%d, %%%d\n",
                 vReg, instId, src1VReg, src2VReg);
      } else if (srcs.size() == 1) {

        auto src1VReg = getVReg(srcs[0]);
        auto vReg = allocVReg(node);

        snprintf(instBuf, instBufSize, "    %%%d = inf_cmp1 %d, %%%d\n", vReg,
                 instId, src1VReg);
      }

      s << instBuf;
      break;
    }

    default: {
      panic("Unsupported Node %s in MIR.\n", *node);
    }
    }
  }

  s << "    %r2 = COPY %" << (usedVReg - 1) << "\n";
  s << "\n";
  s << "...\n";
  s << "\n";
  auto input = s.str();

  auto directory = this->manager->getTDFGFolder();
  auto dumpCount = this->manager->allocDumpCount("uni.");

  std::string inputMIRFn = "tdfg." + std::to_string(dumpCount) + ".raw.mir";
  auto log = directory->create(inputMIRFn);
  (*log->stream()) << input;
  directory->close(log);

  // Approximate number of available registers.
  const auto cacheWordlines = StreamNUCAMap::getCacheParams().wordlines;
  const auto cacheBitlines = StreamNUCAMap::getCacheParams().bitlines;
  const auto tileSize =
      context.pumDataGraphNodes.front()->pumTile.getCanonicalTotalTileSize();
  const auto tileRows = (tileSize + cacheBitlines - 1) / cacheBitlines;
  const auto dataTypeBits =
      context.pumDataGraphNodes.front()->scalarElemSize * 8;
  auto nRegs = cacheWordlines / dataTypeBits / tileRows;

  // Invoke unison.
  auto absoluteInputMIRFn = directory->resolve(inputMIRFn);

  snprintf(instBuf, instBufSize,
           "uni run %s --goal=speed --target=InfStream --targetoption=regs:%ld",
           absoluteInputMIRFn.c_str(), nRegs);

  PUM_LOG_("[UNI] Exec %s\n", instBuf);
  std::string output;
  if (auto prevSol = this->searchPrevUnisonSolution(nRegs, input)) {
    output = prevSol->sol;
  } else {
    output = exec(instBuf);
    this->memorizeUnisonSolution(nRegs, input, output);
  }
  PUM_LOG_("%s\n", output);

  // Write to file.
  std::string outputMIRFn = "tdfg." + std::to_string(dumpCount) + ".uni.mir";
  auto outF = directory->create(outputMIRFn);
  auto &outS = *outF->stream();
  outS << output;

  /**
   * Parse the scheduled tDFG with super adhoc parser.
   */
  MLCPUMManager::PUMDataGraphNodeVec scheduledNodes;
  {

    auto isInstLine = [](const std::string &line) -> bool {
      return line.find(" = ") != std::string::npos;
    };
    auto getInstId = [](const std::string &line) -> int {
      auto instPos = line.find("inf_");
      auto spacePos = line.find(" ", instPos);
      auto commaPos = line.find(",", spacePos);
      if (commaPos == std::string::npos) {
        commaPos = line.size();
      }
      auto instIdStr = line.substr(spacePos + 1, commaPos - spacePos - 1);
      return std::stoi(instIdStr);
    };

    std::istringstream o(output);
    std::string line;
    while (std::getline(o, line)) {
      if (!isInstLine(line)) {
        continue;
      }
      auto instId = getInstId(line);
      auto node = instIdToNodeMap.at(instId);
      scheduledNodes.push_back(node);
    }

    // Add back sync nodes.
    scheduledNodes = this->insertSyncNodes(context, scheduledNodes);

    // Dump scheduled nodes.
    outS << "--- Scheduled tDFG ---\n";
    for (auto node : scheduledNodes) {
      outS << *node << "\n";
    }
  }

  directory->close(outF);
  return scheduledNodes;

  // return this->schedulePUMDataGraphBFS(context, true /* addSync */);
}

void PUMScheduler::memorizeUnisonSolution(int numRegs, const std::string &raw,
                                          const std::string &sol) {
  this->memorizedUnisonSolutions.emplace_back();
  auto &newSol = this->memorizedUnisonSolutions.back();
  newSol.numRegs = numRegs;
  newSol.raw = raw;
  newSol.sol = sol;
}

const PUMScheduler::TriedUnisonSolution *
PUMScheduler::searchPrevUnisonSolution(int numRegs,
                                       const std::string &raw) const {

  for (const auto &sol : this->memorizedUnisonSolutions) {
    if (sol.numRegs == numRegs && sol.raw == raw) {
      PUM_LOG_("[UNI] Reuse Prev Solution\n");
      return &sol;
    }
  }
  return nullptr;
}