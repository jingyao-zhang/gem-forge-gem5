#include "region_stats.hh"

#include "base/cprintf.hh"
#include "base/trace.hh"
#include "debug/RegionStats.hh"

const RegionStats::BasicBlockId RegionStats::InvalidBB = 0;

RegionStats::RegionStats(const RegionTable &_regionTable,
                         const std::string &_fileName)
    : regionTable(_regionTable), fileName(_fileName),
      checkpointsDirectory(nullptr), checkpointsTaken(0),
      previousBB(InvalidBB) {
  // Create the checkpoints directory.
  this->checkpointsDirectory = simout.createSubdirectory("checkpoints");
}

void RegionStats::update(const BasicBlockId &bb) {
  if (bb == this->previousBB) {
    // If we are still in the same block, just return.
    return;
  }

  Snapshot snapshot = nullptr;

  // DPRINTF(RegionStats, "Entered bb %u.\n", bb);
  this->previousBB = bb;

  /**
   * Check if we have exited any active regions.
   */
  for (auto activeIter = this->activeRegions.begin(),
            activeEnd = this->activeRegions.end();
       activeIter != activeEnd;) {
    const auto &region = activeIter->first;
    if (!this->regionTable.isBBInRegion(bb, region)) {
      // We have exited an active region.
      if (!snapshot) {
        snapshot = this->takeSnapshot();
      }
      DPRINTF(RegionStats, "Exit region %s.\n", region.c_str());
      auto statsIter = this->regionStats.find(region);
      if (statsIter == this->regionStats.end()) {
        statsIter =
            this->regionStats
                .emplace(std::piecewise_construct,
                         std::forward_as_tuple(region), std::forward_as_tuple())
                .first;
      }
      this->updateStats(activeIter->second, snapshot, statsIter->second);
      // Erase the active region.
      activeIter = this->activeRegions.erase(activeIter);
    } else {
      // This region is still active.
      ++activeIter;
    }
  }

  // Check if we are in any new region.
  if (!this->regionTable.hasRegionSetFromBB(bb)) {
    return;
  }
  const auto &regionSet = this->regionTable.getRegionSetFromBB(bb);
  for (const auto &newRegion : regionSet) {
    if (this->activeRegions.find(newRegion->name()) ==
        this->activeRegions.end()) {
      // This is a new region.
      DPRINTF(RegionStats, "Enter region %s.\n", newRegion->name().c_str());
      if (!snapshot) {
        snapshot = this->takeSnapshot();
      }
      this->activeRegions.emplace(newRegion->name(), snapshot);
    }
  }
}

void RegionStats::checkpoint(const std::string &suffix) {
  auto fn = std::string("ck.") + std::to_string(this->checkpointsTaken++) +
            "." + suffix + ".txt";
  auto outputStream = this->checkpointsDirectory->findOrCreate(fn);
  auto snapshot = this->takeSnapshot();
  this->dumpStatsMap(*snapshot, *outputStream->stream());
  this->checkpointsDirectory->close(outputStream);
}

RegionStats::Snapshot RegionStats::takeSnapshot() {
  auto snapshot = std::make_shared<StatsMap>();
  auto &stats = Stats::statsList();
  // First we have to prepare all of them.
  for (auto stat : stats) {
    stat->enable();
  }
  for (auto stat : stats) {
    stat->prepare();
  }
  for (auto stat : stats) {
    stat->prepare();
    auto *vector = dynamic_cast<Stats::VectorInfo *>(stat);
    Stats::ScalarInfo *scalar = dynamic_cast<Stats::ScalarInfo *>(stat);
    if (scalar) {
      // We only care about scalar statistics.
      snapshot->emplace(scalar->name, scalar->result());
    } else if (vector) {
      // DPRINTF(RegionStats, "Get stats %f\n", vector->total());
      snapshot->emplace(vector->name, vector->total());
    }
  }
  return snapshot;
}

void RegionStats::updateStats(const Snapshot &enterSnapshot,
                              const Snapshot &exitSnapshot,
                              StatsMap &updatingMap) {
  for (const auto &stat : *enterSnapshot) {
    const auto &statName = stat.first;
    auto exitIter = exitSnapshot->find(statName);
    if (exitIter == exitSnapshot->end()) {
      panic("Missing stat %s in exit snapshot.\n", statName.c_str());
    }
    auto enterValue = stat.second;
    auto exitValue = exitIter->second;
    auto diffValue = exitValue - enterValue;
    auto updateIter = updatingMap.find(statName);
    // Updating the map.
    if (updateIter == updatingMap.end()) {
      updatingMap.emplace(statName, diffValue);
    } else {
      updateIter->second = updateIter->second + diffValue;
    }
  }
  // Add our own region entered statistics.
  auto updateIter = updatingMap.find("region.entered");
  if (updateIter == updatingMap.end()) {
    updatingMap.emplace("region.entered", 1.0);
  } else {
    updateIter->second += 1.0;
  }
}

void RegionStats::dump(std::ostream &stream) {
  // Whenever dump, we add an "all" region.
  auto snapshot = this->takeSnapshot();
  // Add our own hack of region entered statistic.
  this->regionStats["all"] = *snapshot;
  this->regionStats["all"].emplace("region.entered", 1.0);

  for (const auto &regionStat : this->regionStats) {
    const auto &name = regionStat.first;
    ccprintf(stream, "---- %s\n", name);
    // As additional information, we also print region's parent.
    if (name != "all") {
      const auto &region = this->regionTable.getRegionFromRegionId(name);
      ccprintf(stream, "-parent %s\n", region.parent());
    } else {
      // As a special region, all is not in our regions map.
      // Generate an empty parent for "all" region.
      ccprintf(stream, "-parent \n");
    }

    this->dumpStatsMap(regionStat.second, stream);
  }

  this->regionStats.erase("all");
}

void RegionStats::dump() {
  auto outputStream = simout.findOrCreate(this->fileName);
  auto &stream = *outputStream->stream();
  this->dump(stream);
  simout.close(outputStream);
}

void RegionStats::dumpStatsMap(const StatsMap &stats,
                               std::ostream &stream) const {
  // We have to sort this.
  std::map<std::string, Stats::Result> sorted;
  for (const auto &stat : stats) {
    sorted.emplace(stat.first, stat.second);
  }
  for (const auto &stat : sorted) {
    // When dump we ignore nan.
    if (!std::isnan(stat.second)) {
      ccprintf(stream, "%-40s %12f\n", stat.first, stat.second);
    }
  }
}