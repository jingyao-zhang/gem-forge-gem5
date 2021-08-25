#include "LLCStreamMigrationController.hh"

#include "LLCStreamEngine.hh"

#include "debug/LLCRubyStreamBase.hh"
#define DEBUG_TYPE LLCRubyStreamBase
#include "../stream_log.hh"

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
  if (!dynS->getStaticStream()->isStoreComputeStream()) {
    // Try only limit StoreComputeStream.
    return true;
  }
  // Check if the neighboring SE has too many streams.
  auto neighborStreams = this->countStreamsWithSameStaticId(dynS, machineId);
  if (neighborStreams > this->neighborStreamsThreshold) {
    auto ratio = static_cast<float>(neighborStreams) /
                 static_cast<float>(this->neighborStreamsThreshold);
    auto delay = Cycles(
        static_cast<uint64_t>(static_cast<uint64_t>(this->delay) * ratio));
    auto curCycle = this->controller->curCycle();
    auto lastMigratedCycle = this->lastMigratedCycle.at(neighborIdx);
    if (curCycle - lastMigratedCycle < delay) {
      LLC_S_DPRINTF(dynS->getDynamicStreamId(),
                    "[Migrate] Delayed migration to LLC_SE %d to avoid "
                    "contention. NeighborStreams %d.\n",
                    machineId.getNum(), neighborStreams);
      return false;
    }
  }
  return true;
}

int LLCStreamMigrationController::getNeighborIndex(MachineID machineId) const {
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