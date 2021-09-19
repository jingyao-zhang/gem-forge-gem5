#ifndef __GEM_FORGE_LLC_STREAM_MIGRATION_CONTROLLER_HH__
#define __GEM_FORGE_LLC_STREAM_MIGRATION_CONTROLLER_HH__

#include "LLCDynamicStream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include <array>
#include <unordered_set>

/**
 * This is used to limit the migration bandwidth to avoid overwhelming
 * neighboring LLC SE and severe contention.
 */

class LLCStreamMigrationController {
public:
  LLCStreamMigrationController(AbstractStreamAwareController *_controller,
                               int _neighborStreamsThreshold, Cycles _delay);

  void startMigrateTo(LLCDynamicStreamPtr dynS, MachineID machineId);
  bool canMigrateTo(LLCDynamicStreamPtr dynS, MachineID machineId);
  void migratedTo(LLCDynamicStreamPtr dynS, MachineID machineId);

  int curRemoteBank() const {
    return this->controller->getMachineID().getNum();
  }
  const char *curRemoteMachineType() const {
    return this->controller->getMachineTypeString();
  }

private:
  AbstractStreamAwareController *controller;
  const int neighborStreamsThreshold;
  const Cycles delay;

  enum MigrationValveTypeE {
    NONE,
    ALL,
    HARD,
  };
  MigrationValveTypeE valveType;

  static constexpr int MaxNeighbors = 6;
  std::array<Cycles, MaxNeighbors> lastMigratedCycle;

  std::array<std::unordered_set<DynamicStreamId, DynamicStreamIdHasher>,
             MaxNeighbors>
      migratingStreams;

  /**
   * @return -1 if not neighboring.
   */
  int getNeighborIndex(MachineID machineId) const;

  /**
   * Get LLC or MC NeighborIndex.
   * @return -1 if not neighboring.
   */
  int getLLCNeighborIndex(MachineID machineId) const;
  int getMCCNeighborIndex(MachineID machineId) const;

  /**
   * Count the number of migrating and migrated streams with the same StaticId
   * at the destination.
   */
  int countStreamsWithSameStaticId(LLCDynamicStreamPtr dynS,
                                   MachineID machineId) const;
};

#endif
