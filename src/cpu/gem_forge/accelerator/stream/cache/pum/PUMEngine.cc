#include "PUMEngine.hh"
#include "MLCPUMManager.hh"

#include "debug/StreamPUM.hh"

#define DEBUG_TYPE StreamPUM
#include "../../stream_log.hh"

#define LLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(StreamPUM, "[LLC_SE%d]: [PUM] " format,                              \
          this->controller->getMachineID().num, ##args)

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

  LLCSE_DPRINTF("Configured.\n");

  assert(this->nextCmdIdx == this->commands.size() &&
         "Not done with previous commands.");
  this->pumManager = pumManager;
  this->nextCmdIdx = 0;
  this->commands = commands;
  this->sentInterBankPackets = 0;
  this->acked = false;
}

void PUMEngine::kickNextCommand() {

  auto myBankIdx = this->getBankIdx();
  const auto &command = this->commands.at(this->nextCmdIdx);
  this->nextCmdIdx++;
  Cycles latency(1);

  LLC_SE_DPRINTF("Kick NextCmd %s", command);
  const auto &llcCmd = command.llc_commands.at(myBankIdx);
  if (llcCmd.empty()) {
    LLC_SE_DPRINTF("Skip Cmd.\n");
  } else {

    /**
     * Estimate the latency of each command.
     */
    latency = this->estimateCommandLatency(command);
    LLC_SE_DPRINTF("Latency %lld.\n", latency);
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
      LLC_SE_DPRINTF("InterArray Level %d Arrays %d AccLat -> %d.\n", level,
                     levelArrays, accumulatedLatency);
    }

    // Packetize the last inter-bank level (assume 64B data packet).
    const auto packetDataBits = 512;
    for (const auto &entry : interBankBitlineTraffic) {
      auto dstBankIdx = entry.first;
      auto bitlines = entry.second;
      auto totalBits = bitlines * command.wordline_bits;
      auto totalPackets = 0;

      MachineID dstMachineId(MachineType_L2Cache, dstBankIdx);
      for (auto i = 0; i < totalBits; i += packetDataBits, totalPackets += 1) {

        auto bits = packetDataBits;
        if (i + bits > totalBits) {
          bits = totalBits - bits;
        }

        auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
        msg->m_addr = 0;
        msg->m_Type = CoherenceRequestType_STREAM_PUM_DATA;
        msg->m_Requestors.add(this->controller->getMachineID());
        msg->m_Destination.add(dstMachineId);
        msg->m_MessageSize =
            this->controller->getMessageSizeType((bits + 7) / 8);

        this->se->streamIndirectIssueMsgBuffer->enqueue(
            msg, this->controller->clockEdge(),
            this->controller->cyclesToTicks(Cycles(1)));
      }
      this->sentInterBankPackets += totalPackets;
    }

    return Cycles(accumulatedLatency);
  }

  panic("Unknown PUMCommand %s.", command.type);
}

void PUMEngine::tick() {
  if (this->controller->curCycle() < this->nextCmdReadyCycle) {
    return;
  }

  if (this->nextCmdIdx == this->commands.size()) {
    if (!this->acked) {
      this->pumManager->reachSync(this->sentInterBankPackets);
      this->acked = true;
    }
    return;
  }

  this->kickNextCommand();
}

void PUMEngine::receiveData(const RequestMsg &msg) {
  assert(this->pumManager);
  this->pumManager->receivePacket();
}