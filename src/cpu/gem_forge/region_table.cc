#include "region_table.hh"

#include "base/logging.hh"

RegionTable::RegionTable(const LLVM::TDG::StaticInformation &info) {
  // Construct the region map.
  for (const auto &region : info.regions()) {
    const auto &regionId = region.name();
    if (this->regions.find(regionId) != this->regions.end()) {
      panic("Multiple defined region %s.\n", regionId.c_str());
    }
    this->regions.emplace(regionId, &region);
  }
  // Construct the reverse map from bb to region set.
  for (const auto &entry : this->regions) {
    for (const auto &bb : entry.second->bbs()) {
      auto iter = this->bbToRegionMap.find(bb);
      if (iter == this->bbToRegionMap.end()) {
        iter = this->bbToRegionMap
                   .emplace(std::piecewise_construct, std::forward_as_tuple(bb),
                            std::forward_as_tuple())
                   .first;
      }
      iter->second.insert(entry.second);
    }
  }
}

const RegionTable::Region &
RegionTable::getRegionFromRegionId(const RegionId &regionId) const {
  return *(this->regions.at(regionId));
}

bool RegionTable::isBBInRegion(BasicBlockId bbId,
                               const RegionId &regionId) const {
  // ! Currently it takes too much overhead to search through the list.
  // ! Maybe make it a set.
  panic("isBBInRegion not working right now.");
  return false;
}

bool RegionTable::hasRegionSetFromBB(BasicBlockId bbId) const {
  return this->bbToRegionMap.count(bbId) != 0;
}

const RegionTable::RegionSet &
RegionTable::getRegionSetFromBB(BasicBlockId bbId) const {
  return this->bbToRegionMap.at(bbId);
}