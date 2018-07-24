#include "region_stats.hh"

#include "base/cprintf.hh"
#include "base/trace.hh"
#include "debug/RegionStats.hh"

const RegionStats::BasicBlockId RegionStats::InvalidBB = 0;

RegionStats::RegionStats(RegionMap &&_regions, const std::string &_fileName)
    : regions(std::move(_regions)), fileName(_fileName), previousBB(InvalidBB) {
  // Compute the reverse basic block to region map.
  for (const auto &entry : this->regions) {
    const auto &region = entry.first;
    for (const auto &bb : entry.second.bbs) {
      auto iter = this->bbToRegionMap.find(bb);
      if (iter == this->bbToRegionMap.end()) {
        iter = this->bbToRegionMap
                   .emplace(std::piecewise_construct, std::forward_as_tuple(bb),
                            std::forward_as_tuple())
                   .first;
      }
      iter->second.insert(region);
    }
  }
}

void RegionStats::update(const BasicBlockId &bb) {
  if (bb == this->previousBB) {
    // If we are still in the same block, just return.
    return;
  }

  Snapshot snapshot = nullptr;

  /**
   * Check if we have exited any active regions.
   */
  for (auto activeIter = this->activeRegions.begin(),
            activeEnd = this->activeRegions.end();
       activeIter != activeEnd;) {
    const auto &region = activeIter->first;
    if (!this->contains(region, bb)) {
      // We have exited an active region.
      if (!snapshot) {
        snapshot = this->takeSnapshot();
      }
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
  auto bbToRegionMapIter = this->bbToRegionMap.find(bb);
  if (bbToRegionMapIter == this->bbToRegionMap.end()) {
    return;
  }
  for (const auto &newRegion : bbToRegionMapIter->second) {
    if (this->activeRegions.find(newRegion) == this->activeRegions.end()) {
      // This is a new region.
      if (!snapshot) {
        snapshot = this->takeSnapshot();
      }
      this->activeRegions.emplace(newRegion, snapshot);
    }
  }
}

bool RegionStats::contains(const RegionId &region,
                           const BasicBlockId &bb) const {
  const auto &bbs = this->regions.at(region).bbs;
  return bbs.find(bb) != bbs.end();
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
      DPRINTF(RegionStats, "Get stats %f\n", vector->total());
      snapshot->emplace(vector->name, vector->total());
    }
  }
  return snapshot;
}

void RegionStats::updateStats(const Snapshot &enterSnapshot,
                              const Snapshot &exitSnapshot,
                              StatsMap &updatingMap) {
  for (const auto &stat : *enterSnapshot) {
    const auto &name = stat.first;
    auto exitIter = exitSnapshot->find(name);
    if (exitIter == exitSnapshot->end()) {
      panic("Missing stat %s in exit snapshot.\n", name.c_str());
    }
    auto enterValue = stat.second;
    auto exitValue = exitIter->second;
    auto diffValue = exitValue - enterValue;
    auto updateIter = updatingMap.find(name);
    // Updating the map.
    if (updateIter == updatingMap.end()) {
      updatingMap.emplace(name, diffValue);
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

/***********************************************************************************
 * Copy from text.cc
 */

// namespace {
// using namespace Stats;
// string ValueToString(Result value, int precision) {
//   stringstream val;

//   if (!std::isnan(value)) {
//     if (precision != -1)
//       val.precision(precision);
//     else if (value == rint(value))
//       val.precision(0);

//     val.unsetf(ios::showpoint);
//     val.setf(ios::fixed);
//     val << value;
//   } else {
//     val << "nan";
//   }

//   return val.str();
// }

// struct ScalarPrint {
//   Result value;
//   string name;
//   string desc;
//   Flags flags;
//   bool descriptions;
//   int precision;
//   Result pdf;
//   Result cdf;

//   void update(Result val, Result total);
//   void operator()(ostream &stream, bool oneLine = false) const;
// };

// void ScalarPrint::update(Result val, Result total) {
//   value = val;
//   if (total) {
//     pdf = val / total;
//     cdf += pdf;
//   }
// }

// void ScalarPrint::operator()(ostream &stream, bool oneLine) const {
//   if ((flags.isSet(nozero) && (!oneLine) && value == 0.0) ||
//       (flags.isSet(nonan) && std::isnan(value)))
//     return;

//   stringstream pdfstr, cdfstr;

//   if (!std::isnan(pdf))
//     ccprintf(pdfstr, "%.2f%%", pdf * 100.0);

//   if (!std::isnan(cdf))
//     ccprintf(cdfstr, "%.2f%%", cdf * 100.0);

//   if (oneLine) {
//     ccprintf(stream, " |%12s %10s %10s", ValueToString(value, precision),
//              pdfstr.str(), cdfstr.str());
//   } else {
//     ccprintf(stream, "%-40s %12s %10s %10s", name,
//              ValueToString(value, precision), pdfstr.str(), cdfstr.str());

//     if (descriptions) {
//       if (!desc.empty())
//         ccprintf(stream, " # %s", desc);
//     }
//     stream << endl;
//   }
// }

// struct VectorPrint {
//   string name;
//   string separatorString;
//   string desc;
//   vector<string> subnames;
//   vector<string> subdescs;
//   Flags flags;
//   bool descriptions;
//   int precision;
//   VResult vec;
//   Result total;
//   bool forceSubnames;

//   void operator()(ostream &stream) const;
// };

// void VectorPrint::operator()(std::ostream &stream) const {
//   size_type _size = vec.size();
//   Result _total = 0.0;

//   if (flags.isSet(pdf | cdf)) {
//     for (off_type i = 0; i < _size; ++i) {
//       _total += vec[i];
//     }
//   }

//   string base = name + separatorString;

//   ScalarPrint print;
//   print.name = name;
//   print.desc = desc;
//   print.precision = precision;
//   print.descriptions = descriptions;
//   print.flags = flags;
//   print.pdf = _total ? 0.0 : NAN;
//   print.cdf = _total ? 0.0 : NAN;

//   bool havesub = !subnames.empty();

//   if (_size == 1) {
//     // If forceSubnames is set, get the first subname (or index in
//     // the case where there are no subnames) and append it to the
//     // base name.
//     if (forceSubnames)
//       print.name = base + (havesub ? subnames[0] : std::to_string(0));
//     print.value = vec[0];
//     print(stream);
//     return;
//   }

//   if ((!flags.isSet(nozero)) || (total != 0)) {
//     if (flags.isSet(oneline)) {
//       ccprintf(stream, "%-40s", name);
//       print.flags = print.flags & (~nozero);
//     }

//     for (off_type i = 0; i < _size; ++i) {
//       if (havesub && (i >= subnames.size() || subnames[i].empty()))
//         continue;

//       print.name = base + (havesub ? subnames[i] : std::to_string(i));
//       print.desc = subdescs.empty() ? desc : subdescs[i];

//       print.update(vec[i], _total);
//       print(stream, flags.isSet(oneline));
//     }

//     if (flags.isSet(oneline)) {
//       if (descriptions) {
//         if (!desc.empty())
//           ccprintf(stream, " # %s", desc);
//       }
//       stream << endl;
//     }
//   }

//   if (flags.isSet(::Stats::total)) {
//     print.pdf = NAN;
//     print.cdf = NAN;
//     print.name = base + "total";
//     print.desc = desc;
//     print.value = total;
//     print(stream);
//   }
// }
// } // namespace

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
      const auto &region = this->regions.at(name);
      ccprintf(stream, "-parent %s\n", region.parent);
    } else {
      // As a special region, all is not in our regions map.
      // Generate an empty parent for "all" region.
      ccprintf(stream, "-parent \n");
    }
    // We have to sort this.
    std::map<std::string, Stats::Result> sorted;
    for (const auto &stat : regionStat.second) {
      sorted.emplace(stat.first, stat.second);
    }
    for (const auto &stat : sorted) {
      // When dump we ignore nan.
      if (!std::isnan(stat.second)) {
        ccprintf(stream, "%-40s %12f\n", stat.first, stat.second);
      }
    }
  }

  this->regionStats.erase("all");
}

void RegionStats::dump() {
  auto &stream = *simout.findOrCreate(this->fileName)->stream();
  this->dump(stream);
}