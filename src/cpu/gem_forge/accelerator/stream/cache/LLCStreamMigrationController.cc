#include "LLCStreamMigrationController.hh"

#include "LLCStreamEngine.hh"

#include "debug/LLCStreamMigrationController.hh"
#define DEBUG_TYPE LLCStreamMigrationController
#include "../stream_log.hh"

const int LLCStreamMigrationController::MaxNeighbors;

LLCStreamMigrationController::LLCStreamMigrationController(
    AbstractStreamAwareController *_controller, int _neighborStreamsThreshold,
    Cycles _delay)
    : controller(_controller),
      neighborStreamsThreshold(_neighborStreamsThreshold), delay(_delay) {
  for (auto &c : this->lastMigratedCycle) {
    c = Cycles(0);
  }
  for (auto &c : this->migratingStreams) {
    c = std::unordered_set<DynamicStreamId, DynamicStreamIdHasher>();
  }
  const auto &valveTypeStr =
      _controller->myParams->neighbor_migration_valve_type;
  if (valveTypeStr == "none") {
    this->valveType = MigrationValveTypeE::NONE;
  } else if (valveTypeStr == "all") {
    this->valveType = MigrationValveTypeE::ALL;
  } else if (valveTypeStr == "hard") {
    this->valveType = MigrationValveTypeE::HARD;
  } else {
    panic("Unknown MigrationValveType %s.", valveTypeStr);
  }
}

void LLCStreamMigrationController::startMigrateTo(LLCDynamicStreamPtr dynS,
                                                  MachineID machineId) {
  if (this->neighborStreamsThreshold == 0) {
    // I am disabled.
    return;
  }
  auto neighborIdx = this->getNeighborIndex(machineId);
  if (neighborIdx == -1) {
    return;
  }
  this->lastMigratedCycle.at(neighborIdx) = this->controller->curCycle();
  this->migratingStreams.at(neighborIdx).insert(dynS->getDynamicStreamId());
}

void LLCStreamMigrationController::migratedTo(LLCDynamicStreamPtr dynS,
                                              MachineID machineId) {
  if (this->neighborStreamsThreshold == 0) {
    // I am disabled.
    return;
  }
  auto neighborIdx = this->getNeighborIndex(machineId);
  if (neighborIdx == -1) {
    return;
  }
  this->migratingStreams.at(neighborIdx).erase(dynS->getDynamicStreamId());
}

bool LLCStreamMigrationController::canMigrateTo(LLCDynamicStreamPtr dynS,
                                                MachineID machineId) {
  if (this->neighborStreamsThreshold == 0) {
    // I am disabled.
    return true;
  }
  auto neighborIdx = this->getNeighborIndex(machineId);
  if (neighborIdx == -1) {
    // Not my neighbor, we have no limitation to migrating there.
    return true;
  }
  if (this->valveType != MigrationValveTypeE::ALL &&
      !dynS->isLoadBalanceValve()) {
    // For MC, always enable this feature.
    return true;
  }
  // Check if the neighboring SE has too many streams.
  auto neighborStreams = this->countStreamsWithSameStaticId(dynS, machineId);
  if (neighborStreams > this->neighborStreamsThreshold) {
    if (this->valveType == MigrationValveTypeE::HARD) {
      LLC_S_DPRINTF(dynS->getDynamicStreamId(),
                    "[Migrate] Hard Delayed Migration to %s to avoid "
                    "contention. NeighborStreams %d.\n",
                    machineId, neighborStreams);
      return false;
    }
    auto ratio = static_cast<float>(neighborStreams) /
                 static_cast<float>(this->neighborStreamsThreshold);
    auto delay = Cycles(
        static_cast<uint64_t>(static_cast<uint64_t>(this->delay) * ratio));
    auto curCycle = this->controller->curCycle();
    auto lastMigratedCycle = this->lastMigratedCycle.at(neighborIdx);
    if (curCycle - lastMigratedCycle < delay) {
      LLC_S_DPRINTF(dynS->getDynamicStreamId(),
                    "[Migrate] Delayed migration to %s to avoid "
                    "contention. NeighborStreams %d.\n",
                    machineId, neighborStreams);
      dynS->getStaticS()->statistic.numRemoteMigrateDelayCycle++;
      return false;
    }
  }
  return true;
}

int LLCStreamMigrationController::getLLCNeighborIndex(
    MachineID machineId) const {
  auto myBank = this->curRemoteBank();
  auto bank = machineId.getNum();
  if (this->controller->isMyNeighbor(machineId)) {
    if (bank + 1 < myBank) {
      return 0;
    } else if (bank + 1 == myBank) {
      return 1;
    } else if (myBank + 1 == bank) {
      return 2;
    } else {
      return 3;
    }
  } else {
    return -1;
  }
}

int LLCStreamMigrationController::getMCCNeighborIndex(
    MachineID machineId) const {
  auto myBank = this->curRemoteBank();
  auto bank = machineId.getNum();
  /**
   * This should really depend on the topology. But so far
   * we assume the memory controller are all connected.
   * And we just take left 3 controllers and right 3 controller.
   */
  auto halfNeighborDist = MaxNeighbors / 2;
  if ((bank + halfNeighborDist < myBank) ||
      (myBank + halfNeighborDist < bank)) {
    return -1;
  } else {
    if (bank < myBank) {
      return myBank - bank - 1;
    } else if (bank > myBank) {
      return bank - myBank - 1 + halfNeighborDist;
    } else {
      panic("MC Bank should not be the same %d != %d.", myBank, bank);
    }
  }
}

int LLCStreamMigrationController::getNeighborIndex(MachineID machineId) const {
  auto myMachineId = this->controller->getMachineID();
  if (myMachineId.getType() != machineId.getType()) {
    return -1;
  }
  if (machineId == myMachineId) {
    panic("Self Stream Migration %s -> %s is illegal.", myMachineId, machineId);
  }
  auto neighborIndex = -1;
  switch (myMachineId.getType()) {
  case MachineType::MachineType_L2Cache: {
    neighborIndex = this->getLLCNeighborIndex(machineId);
    break;
  }
  case MachineType::MachineType_Directory: {
    neighborIndex = this->getMCCNeighborIndex(machineId);
    break;
  }
  default: {
    panic("Illegal Machine to Migrate Streams %s.", myMachineId);
  }
  }
  if (neighborIndex >= MaxNeighbors) {
    panic("NeighborIndex Overflow %d >= %d.", neighborIndex, MaxNeighbors);
  }
  return neighborIndex;
}

int LLCStreamMigrationController::countStreamsWithSameStaticId(
    LLCDynamicStreamPtr dynS, MachineID machineId) const {
  auto neighborSE = AbstractStreamAwareController::getController(machineId)
                        ->getLLCStreamEngine();
  auto neighborStreams =
      neighborSE->getNumDirectStreamsWithStaticId(dynS->getDynamicStreamId());

  auto neighborIdx = this->getNeighborIndex(machineId);
  for (const auto &migratingDynStreamId :
       this->migratingStreams.at(neighborIdx)) {
    if (migratingDynStreamId.staticId == dynS->getDynamicStreamId().staticId) {
      neighborStreams++;
    }
  }
  return neighborStreams;
}