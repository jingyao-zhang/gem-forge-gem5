#ifndef __CPU_LLVM_TRACE_REGION_STATS_H__
#define __CPU_LLVM_TRACE_REGION_STATS_H__

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
  using RegionId = std::string;
  using BasicBlockId = uint64_t;
  using RegionMap =
      std::unordered_map<RegionId, std::unordered_set<BasicBlockId>>;

  using StatsMap = std::unordered_map<std::string, Stats::Result>;
  using Snapshot = std::shared_ptr<const StatsMap>;

  RegionStats(RegionMap &&_regions, const std::string &_fileName);

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

  void dump(std::ostream &stream);
  void dump();

private:
  RegionMap regions;
  std::string fileName;

  // Reverse map from basic block to regions to speed up the look up.
  std::unordered_map<BasicBlockId, std::unordered_set<RegionId>> bbToRegionMap;

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
   * Check if a region contains a bb.
   */
  bool contains(const RegionId &region, const BasicBlockId &bb) const;

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
};

#endif