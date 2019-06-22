#ifndef __CPU_GEM_FORGE_REGION_STATS_HH__
#define __CPU_GEM_FORGE_REGION_STATS_HH__

#include "region_table.hh"

#include "base/output.hh"
#include "base/statistics.hh"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

/**
 * This class collects statistics for regions.
 * Each region contains a list of basic blocks. When we enter a region, we take
 * an enter_snapshot. When we leave a region, we take an exit_snapshot. We
 * update the region's statistics with exit_snapshot - start_snapshot.
 *
 * The region should be sufficiently large, otherwise the statistics are not
 * accurate.
 *
 * NOTE: Regions should be closed (continuous), i.e. no function call within the
 * body. Regions can be nested. You can think regions as loops without function
 * call.
 *
 * Currently only supports scalar/formula statistics. Also the statistics should
 * be commulative. It is the user's responsibility to check this.
 *
 * When dumping, if the stats is nan, we simply ignore it to make the stats
 * cleaner.
 */
class RegionStats {
public:
  using RegionId = RegionTable::RegionId;
  using BasicBlockId = RegionTable::BasicBlockId;
  using Region = RegionTable::Region;
  using RegionMap = RegionTable::RegionMap;

  using StatsMap = std::unordered_map<std::string, Stats::Result>;
  using Snapshot = std::shared_ptr<const StatsMap>;

  RegionStats(const RegionTable &_regionTable, const std::string &_fileName);

  RegionStats(const RegionStats &other) = delete;
  RegionStats(RegionStats &&other) = delete;
  RegionStats &operator=(const RegionStats &other) = delete;
  RegionStats &operator=(RegionStats &&other) = delete;

  /**
   * 1. Check if we exit any active regions, take the exit_snapshot and compute
   * the exit_snapshot - enter_snapshot.
   * 2. Check if we enter any new regions, take the enter_snapshot and mark
   * these regions active.
   *
   * 0 is reserved as invalid bb, if passed, indicating the end of stream.
   */
  void update(const BasicBlockId &bb);
  static const BasicBlockId InvalidBB;

  void checkpoint(const std::string &suffix);

  void dump(std::ostream &stream);
  void dump();

private:
  const RegionTable &regionTable;
  std::string fileName;

  OutputDirectory *checkpointsDirectory;
  uint64_t checkpointsTaken;

  // Record the previous basic block to avoid expensive loop up if we are still
  // in the same basic block.
  BasicBlockId previousBB;

  /**
   * Map from active region to the enter_snapshot.
   * Notice that we use ordinary map here, as the number of active regions tends
   * to be small (2 or 3 nested loops).
   *
   * Also we may erase when iterating it, and it is not since C++14 that
   * unordered_map::erase guarantees that the order of unerased elements is
   * preserved.
   */
  std::map<RegionId, Snapshot> activeRegions;

  /**
   * Map from regions to collected statstics.
   */
  std::unordered_map<RegionId, StatsMap> regionStats;

  /**
   * Take the snapshot.
   */
  Snapshot takeSnapshot();

  /**
   * Compute exit_snapshot - enter_snapshot and update stats.
   * If the entry in the update stats is missing, we initialize it to 0.
   */
  void updateStats(const Snapshot &enterSnapshot, const Snapshot &exitSnapshot,
                   StatsMap &updatingMap);

  void dumpStatsMap(const StatsMap &stats, std::ostream &stream) const;
};

#endif