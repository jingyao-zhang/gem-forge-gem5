#include "region_stats.hh"

#include "base/cprintf.hh"
#include "base/trace.hh"
#include "debug/RegionStats.hh"

namespace gem5 {

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
                       std::forward_as_tuple(regionId),
                       std::forward_as_tuple(this->statsVecTemplate.numStats))
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
  // this->dumpStatsVec(*snapshot, *outputStream->stream());
  this->checkpointsDirectory->close(outputStream);
}

void RegionStats::initializeStatsVecTemplate() {
  assert(!this->statsVecTemplate.initialized &&
         "Already initialized StatsVecTemplate.");
  // So far we only care about scalar and vector stats.
  for (auto stat : Stats::statsList()) {
    // ! Crazy template black magic in Stats.
    if (auto scalar = dynamic_cast<ScalarInfo *>(stat)) {
      this->statsVecTemplate.scalarStats.push_back(scalar);
    } else if (auto *vector = dynamic_cast<VectorInfo *>(stat)) {
      this->statsVecTemplate.vectorStats.push_back(vector);
    }
  }
  this->statsVecTemplate.numStats = this->statsVecTemplate.scalarStats.size() +
                                    this->statsVecTemplate.vectorStats.size();
  this->statsVecTemplate.initialized = true;
}

RegionStats::Snapshot RegionStats::takeSnapshot() {
  auto snapshot = std::make_shared<StatsVec>();
  if (!this->statsVecTemplate.initialized) {
    this->initializeStatsVecTemplate();
  }
  snapshot->reserve(this->statsVecTemplate.numStats);
  for (auto scalar : this->statsVecTemplate.scalarStats) {
    scalar->enable();
    scalar->prepare();
    snapshot->push_back(scalar->result());
  }
  for (auto vector : this->statsVecTemplate.vectorStats) {
    vector->enable();
    vector->prepare();
    snapshot->push_back(vector->total());
  }
  return snapshot;
}

void RegionStats::updateStats(const Snapshot &enterSnapshot,
                              const Snapshot &exitSnapshot,
                              StatsVecExt &updatingStats) {
  for (size_t statId = 0, statEnd = enterSnapshot->size(); statId < statEnd;
       ++statId) {
    auto enterValue = enterSnapshot->at(statId);
    auto exitValue = exitSnapshot->at(statId);
    auto diffValue = exitValue - enterValue;
    updatingStats.vec.at(statId) += diffValue;
  }

  // Add our own region entered statistics.
  updatingStats.entered++;
}

void RegionStats::dump(std::ostream &stream) {
  // Whenever dump, we add an "all" region.
  auto snapshot = this->takeSnapshot();
  // Add our own hack of region entered statistic.
  auto &all =
      this->regionStats
          .emplace(std::piecewise_construct,
                   std::forward_as_tuple(RegionTable::REGION_ID_ALL),
                   std::forward_as_tuple(this->statsVecTemplate.numStats))
          .first->second;
  all.vec = *snapshot;
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
    this->dumpStatsVec(regionStat.second, stream);
  }

  this->regionStats.erase(RegionTable::REGION_ID_ALL);
}

void RegionStats::dump() {
  auto outputStream = simout.findOrCreate(this->fileName);
  auto &stream = *outputStream->stream();
  this->dump(stream);
  simout.close(outputStream);
}

void RegionStats::dumpStatsVec(const StatsVecExt &stats,
                               std::ostream &stream) const {
  // We have to sort this.
  std::map<std::string, Stats::Result> sorted;
  for (size_t statId = 0, statEnd = stats.vec.size(); statId < statEnd;
       ++statId) {
    auto value = stats.vec.at(statId);
    auto nScalarStats = this->statsVecTemplate.scalarStats.size();
    if (statId < nScalarStats) {
      sorted.emplace(this->statsVecTemplate.scalarStats.at(statId)->name,
                     value);
    } else {
      sorted.emplace(
          this->statsVecTemplate.vectorStats.at(statId - nScalarStats)->name,
          value);
    }
  }
  // Don't forget our own stats.
  sorted.emplace("region.entered", stats.entered);
  for (const auto &stat : sorted) {
    // When dump we ignore nan.
    if (!std::isnan(stat.second)) {
      ccprintf(stream, "%-40s %12f\n", stat.first, stat.second);
    }
  }
}} // namespace gem5

