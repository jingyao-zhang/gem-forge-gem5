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
    const auto &regionId = activeIter->first;
    if (!this->regionTable.isBBInRegion(bb, regionId)) {
      // We have exited an active region.
      if (!snapshot) {
        snapshot = this->takeSnapshot();
      }
      if (Debug::RegionStats) {
        const auto &region = this->regionTable.getRegionFromRegionId(regionId);
        DPRINTF(RegionStats, "Exit region %s.\n", region.name().c_str());
      }
      auto statsIter =
          this->regionStats
              .emplace(std::piecewise_construct,
                       std::forward_as_tuple(regionId), std::forward_as_tuple())
              .first;
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
  for (const auto &newRegionId : this->regionTable.getRegionSetFromBB(bb)) {
    if (this->activeRegions.find(newRegionId) == this->activeRegions.end()) {
      // This is a new region.
      if (Debug::RegionStats) {
        const auto &newRegion =
            this->regionTable.getRegionFromRegionId(newRegionId);
        DPRINTF(RegionStats, "Enter region %s.\n", newRegion.name().c_str());
      }
      if (!snapshot) {
        snapshot = this->takeSnapshot();
      }
      this->activeRegions.emplace(newRegionId, snapshot);
    }
  }
}

void RegionStats::checkpoint(const std::string &suffix) {
  auto fn = std::string("ck.") + std::to_string(this->checkpointsTaken++) +
            "." + suffix + ".txt";
  auto outputStream = this->checkpointsDirectory->findOrCreate(fn);
  auto snapshot = this->takeSnapshot();
  panic("Checkpoint has too much overhead, disabled for now.\n");
  // this->dumpStatsMap(*snapshot, *outputStream->stream());
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
      snapshot->emplace(stat, scalar->result());
    } else if (vector) {
      // DPRINTF(RegionStats, "Get stats %f\n", vector->total());
      snapshot->emplace(stat, vector->total());
    }
  }
  return snapshot;
}

void RegionStats::updateStats(const Snapshot &enterSnapshot,
                              const Snapshot &exitSnapshot,
                              StatsMapExt &updatingMap) {
  for (const auto &stat : *enterSnapshot) {
    const auto &info = stat.first;
    auto exitIter = exitSnapshot->find(info);
    if (exitIter == exitSnapshot->end()) {
      panic("Missing stat %s in exit snapshot.\n", info->name.c_str());
    }
    auto enterValue = stat.second;
    auto exitValue = exitIter->second;
    auto diffValue = exitValue - enterValue;
    updatingMap.map.emplace(info, 0.0).first->second += diffValue;
  }
  // Add our own region entered statistics.
  updatingMap.entered++;
  // auto updateIter = updatingMap.find("region.entered");
  // if (updateIter == updatingMap.end()) {
  //   updatingMap.emplace("region.entered", 1.0);
  // } else {
  //   updateIter->second += 1.0;
  // }
}

void RegionStats::dump(std::ostream &stream) {
  // Whenever dump, we add an "all" region.
  auto snapshot = this->takeSnapshot();
  // Add our own hack of region entered statistic.
  auto &all = this->regionStats
                  .emplace(std::piecewise_construct,
                           std::forward_as_tuple(RegionTable::REGION_ID_ALL),
                           std::forward_as_tuple())
                  .first->second;
  all.map = *snapshot;
  all.entered = 1;

  for (const auto &regionStat : this->regionStats) {
    const auto &regionId = regionStat.first;
    // As additional information, we also print region's parent.
    if (regionId == RegionTable::REGION_ID_ALL) {
      // As a special region, all is not in our regions map.
      // Generate an empty parent for "all" region.
      ccprintf(stream, "---- all\n");
      ccprintf(stream, "-parent \n");
    } else {
      const auto &region = this->regionTable.getRegionFromRegionId(regionId);
      ccprintf(stream, "---- %s\n", region.name());
      ccprintf(stream, "-parent %s\n", region.parent());
    }
    this->dumpStatsMap(regionStat.second, stream);
  }

  this->regionStats.erase(RegionTable::REGION_ID_ALL);
}

void RegionStats::dump() {
  auto outputStream = simout.findOrCreate(this->fileName);
  auto &stream = *outputStream->stream();
  this->dump(stream);
  simout.close(outputStream);
}

void RegionStats::dumpStatsMap(const StatsMapExt &stats,
                               std::ostream &stream) const {
  // We have to sort this.
  std::map<std::string, Stats::Result> sorted;
  for (const auto &stat : stats.map) {
    sorted.emplace(stat.first->name, stat.second);
  }
  // Don't forget our own stats.
  sorted.emplace("region.entered", stats.entered);
  for (const auto &stat : sorted) {
    // When dump we ignore nan.
    if (!std::isnan(stat.second)) {
      ccprintf(stream, "%-40s %12f\n", stat.first, stat.second);
    }
  }
}