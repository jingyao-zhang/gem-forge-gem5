#include "PUMEngine.hh"
#include "MLCPUMManager.hh"

#include "debug/StreamPUM.hh"

#define DEBUG_TYPE StreamPUM
#include "../../stream_log.hh"

PUMEngine::PUMEngine(LLCStreamEngine *_se)
    : se(_se), controller(_se->controller) {}

void PUMEngine::receiveConfigure(const RequestMsg &msg) {
  assert(this->pumManager && "Not configured yet.");
  assert(this->nextCmdIdx == 0 && "Not configured.");
  this->kickNextCommand();
}

void PUMEngine::configure(MLCPUMManager *pumManager,
                          const PUMCommandVecT &commands) {

  // Initialize HWConfig.
  if (!this->hwConfig) {
    this->hwConfig = m5::make_unique<PUMHWConfiguration>(
        PUMHWConfiguration::getPUMHWConfig());
  }

  LLC_SE_DPRINTF("[PUMEngine] Configured.\n");

  if (this->nextCmdIdx != this->commands.size()) {
    LLC_SE_PANIC("Not done with previous commands. NextCmdIdx %d Commands %d.",
                 this->nextCmdIdx, this->commands.size());
  }

  this->pumManager = pumManager;
  this->nextCmdIdx = 0;
  this->sentInterBankPackets = 0;
  this->acked = false;
  this->commands.clear();

  /**
   * Filter out unrelated commands and merge continuous one.
   */
  auto myBankIdx = this->getBankIdx();
  for (int i = 0; i < commands.size(); ++i) {
    const auto &command = commands[i];
    if (command.type == "sync") {
      // Sync command is always related.
      this->commands.push_back(command);
      continue;
    }
    const auto &llcCmd = command.llcSplitTileCmds.at(myBankIdx);
    if (llcCmd.empty()) {
      continue;
    }
    auto c = commands[i];
    for (auto j = 0; j < c.llcSplitTileCmds.size(); ++j) {
      if (j != myBankIdx) {
        c.llcSplitTileCmds[j].clear();
      }
    }
    this->commands.push_back(c);
  }
}

void PUMEngine::kickNextCommand() {

  auto myBankIdx = this->getBankIdx();
  Cycles latency(1);

  /**
   * As an optimization, we schedule future commands if they are using different
   * arrays. NOTE: This may assume too much schedule flexibility.
   */
  std::unordered_set<int64_t> scheduledArrays;
  auto firstSchedCmdIdx = this->nextCmdIdx;
  while (this->nextCmdIdx < this->commands.size()) {
    const auto &command = this->commands.at(this->nextCmdIdx);

    if (command.type == "sync") {
      // Sync command is handled in wakeup.
      break;
    }

    if (this->nextCmdIdx > firstSchedCmdIdx) {
      const auto &firstSchedCmd = this->commands.at(firstSchedCmdIdx);
      if (firstSchedCmd.type != command.type ||
          firstSchedCmd.opClass != command.opClass) {
        // Cannot schedule different type commands.
        break;
      }
    }

    const auto &llcCmds = command.llcSplitTileCmds.at(myBankIdx);
    assert(!llcCmds.empty() && "Empty LLC command.\n");

    std::unordered_set<int64_t> usedArrays;
    for (const auto &mask : llcCmds) {
      for (auto arrayIdx : mask.srcTilePattern.generate_all_values()) {
        usedArrays.insert(arrayIdx);
      }
    }
    bool conflicted = false;
    for (auto arrayIdx : usedArrays) {
      if (scheduledArrays.count(arrayIdx)) {
        LLC_SE_DPRINTF("  Conflict Array %lld NextCmd %s", arrayIdx, command);
        conflicted = true;
        break;
      }
    }
    if (conflicted) {
      break;
    }

    LLC_SE_DPRINTF("[Kick] NextCmd %s", command.to_string(myBankIdx));

    /**
     * Estimate the latency of each command.
     */
    scheduledArrays.insert(usedArrays.begin(), usedArrays.end());
    auto cmdLat = this->estimateCommandLatency(command);
    this->nextCmdIdx++;
    LLC_SE_DPRINTF("  CMD Latency %lld.\n", cmdLat);
    if (cmdLat > latency) {
      latency = cmdLat;
    }
  }

  this->nextCmdReadyCycle = this->controller->curCycle() + latency;
  this->se->scheduleEvent(latency);
}

Cycles PUMEngine::estimateCommandLatency(const PUMCommand &command) {

  if (command.type == "intra-array") {
    /**
     * Intra array is easy, just charge one cycle for each wordline.
     */
    return Cycles(command.wordline_bits);
  }

  if (command.type == "inter-array") {
    /**
     * Inter array is complicated:
     * For each level:
     * 1. Get the number of bitlines per array.
     * 2. Get the number of arrays need to be transfered in that level.
     * 3. Estimate the latency.
     *
     * Special case is the last level, which is inter-llc-bank traffic.
     * We need to construct packet and send out the fake data.
     * Then we record how many packets are sent out to monitor when they all
     * arrived.
     */
    auto myBankIdx = this->getBankIdx();

    auto llcTreeLeafBandwidthBits = this->hwConfig->tree_leaf_bw_bytes * 8;
    auto bitlinesPerArray = command.bitline_mask.get_total_trip();
    auto latencyPerWordline =
        (bitlinesPerArray + llcTreeLeafBandwidthBits - 1) /
        llcTreeLeafBandwidthBits;
    auto latencyPerArray = latencyPerWordline * command.wordline_bits;

    std::vector<std::pair<int, int>> interBankBitlineTraffic;

    auto accumulatedLatency = 0;
    auto numLevels = command.inter_array_splits.size();
    for (int level = 0; level < numLevels; ++level) {
      auto levelArrays = 0;
      for (const auto &splitPattern : command.inter_array_splits[level]) {
        // TODO: Intersect with LLC array masks.
        if (level + 1 == numLevels) {
          /**
           * This is the last inter-bank level.
           * Notice that PUMEngine is placed at bank level, here we only
           * care about the first trip.
           */
          levelArrays += splitPattern.get_trips().front();

          auto srcArrayIdx = this->hwConfig->get_array_per_bank() * myBankIdx +
                             splitPattern.start;
          auto dstArrayIdx = srcArrayIdx + command.tile_dist;
          if (dstArrayIdx < 0) {
            dstArrayIdx += this->hwConfig->get_total_arrays();
          } else if (dstArrayIdx >= this->hwConfig->get_total_arrays()) {
            dstArrayIdx = dstArrayIdx % this->hwConfig->get_total_arrays();
          }
          auto dstBankIdx =
              this->hwConfig->get_bank_idx_from_array_idx(dstArrayIdx);

          auto numInterBankBitlines =
              splitPattern.get_trips().front() * bitlinesPerArray;
          interBankBitlineTraffic.emplace_back(dstBankIdx,
                                               numInterBankBitlines);

          LLC_SE_DPRINTF("Bank %d -> %d Array %d -> %d Bitlines %d.\n",
                         myBankIdx, dstBankIdx, srcArrayIdx, dstArrayIdx,
                         numInterBankBitlines);

        } else {
          /**
           * Still intra-bank level.
           */
          levelArrays += splitPattern.get_total_trip();
        }
      }
      accumulatedLatency += levelArrays * latencyPerArray;
      LLC_SE_DPRINTF("InterArray Level %d Arrays %d AccLat +%d -> %d.\n", level,
                     levelArrays, levelArrays * latencyPerArray,
                     accumulatedLatency);
    }

    // Packetize the last inter-bank level (assume 64B data packet).
    const auto packetDataBits = 512;
    for (const auto &entry : interBankBitlineTraffic) {
      auto dstBankIdx = entry.first;
      auto bitlines = entry.second;
      auto totalBits = bitlines * command.wordline_bits;
      auto totalPackets = 0;

      NetDest dstBanks;
      if (command.hasReuse()) {
        /**
         * Hack: when there is reuse, just send to all the DstBank.
         * TODO: Properly handle this.
         */
        const auto &dstSplitBanks =
            command.llcSplitTileCmds[myBankIdx][0].dstSplitTilePatterns;
        for (auto dstBankIdx = 0; dstBankIdx < dstSplitBanks.size();
             ++dstBankIdx) {
          if (dstSplitBanks[dstBankIdx].empty()) {
            continue;
          }
          MachineID dstMachineId(MachineType_L2Cache, dstBankIdx);
          dstBanks.add(dstMachineId);
        }

      } else {
        MachineID dstMachineId(MachineType_L2Cache, dstBankIdx);
        dstBanks.add(dstMachineId);
      }

      for (auto i = 0; i < totalBits;
           i += packetDataBits, totalPackets += dstBanks.count()) {

        auto bits = packetDataBits;
        if (i + bits > totalBits) {
          bits = totalBits - bits;
        }

        auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
        msg->m_addr = 0;
        msg->m_Type = CoherenceRequestType_STREAM_PUM_DATA;
        msg->m_Requestors.add(this->controller->getMachineID());
        msg->m_Destination = dstBanks;
        msg->m_MessageSize =
            this->controller->getMessageSizeType((bits + 7) / 8);

        LLC_SE_DPRINTF("[PUMEngine] Send Inter-Bank Data -> %s.\n", dstBanks);

        this->se->streamIndirectIssueMsgBuffer->enqueue(
            msg, this->controller->clockEdge(),
            this->controller->cyclesToTicks(Cycles(1)));
      }
      this->sentInterBankPackets += totalPackets;
    }

    return Cycles(accumulatedLatency);
  }

  if (command.type == "cmp") {
    auto wordlineBits = command.wordline_bits;
    int computeLatency = wordlineBits;
    switch (command.opClass) {
    default:
      panic("Unkown PUM OpClass %s.", Enums::OpClassStrings[command.opClass]);
      break;
    case IntAluOp:
      computeLatency = wordlineBits;
      break;
    case IntMultOp:
      computeLatency = wordlineBits * wordlineBits / 2;
      break;
    case SimdFloatAddOp:
    case SimdFloatMultOp:
    case SimdFloatDivOp:
      computeLatency = wordlineBits * wordlineBits;
      break;
    }
    return Cycles(computeLatency);
  }

  panic("Unknown PUMCommand %s.", command.type);
}

void PUMEngine::tick() {

  if (this->commands.empty()) {
    return;
  }

  if (this->controller->curCycle() < this->nextCmdReadyCycle) {
    return;
  }

  if (this->nextCmdIdx == this->commands.size() ||
      this->commands[this->nextCmdIdx].type == "sync") {
    if (!this->acked) {
      LLC_SE_DPRINTF("[Sync] SentPackets %d.\n", this->sentInterBankPackets);
      auto sentPackets = this->sentInterBankPackets;
      this->acked = true;
      this->sentInterBankPackets = 0;
      this->pumManager->reachSync(sentPackets);
    }
    return;
  }

  this->kickNextCommand();
}

void PUMEngine::synced() {
  assert(this->nextCmdIdx < this->commands.size());
  const auto &c = this->commands[this->nextCmdIdx];
  assert(c.type == "sync");
  assert(this->acked);
  this->acked = false;
  this->nextCmdIdx++;
  this->kickNextCommand();
}

void PUMEngine::receiveData(const RequestMsg &msg) {
  assert(this->pumManager);
  this->pumManager->receivePacket();
}