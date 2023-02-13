#include "region_table.hh"

#include "base/logging.hh"

#define ss(v) #v
#define s(v) ss(v)
#pragma message(s(__cplusplus))

namespace gem5 {

const RegionTable::RegionId RegionTable::REGION_ID_ALL;

RegionTable::RegionTable(const LLVM::TDG::StaticInformation &info) {
  // Construct the region map.
  for (const auto &region : info.regions()) {
    // ! 0 is reserved for "all" region.
    auto regionId = this->regions.size() + 1;
    auto inserted = this->regions.emplace(regionId, &region).second;
    if (!inserted) {
      panic("Multiple defined region %s.\n", region.name().c_str());
    }
  }
  // Construct the reverse map from bb to region set.
  for (const auto &entry : this->regions) {
    for (const auto &bb : entry.second->bbs()) {
      this->bbToRegionMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(bb),
                   std::forward_as_tuple())
          .first->second.insert(entry.first);
    }
  }
}

const RegionTable::Region &
RegionTable::getRegionFromRegionId(const RegionId &regionId) const {
  return *(this->regions.at(regionId));
}

bool RegionTable::isBBInRegion(BasicBlockId bbId,
                               const RegionId &regionId) const {
  if (!this->hasRegionSetFromBB(bbId)) {
    return false;
  }
  auto iter = this->bbToRegionMap.find(bbId);
  if (iter != this->bbToRegionMap.end()) {
    return iter->second.count(regionId) > 0;
  } else {
    return false;
  }
}

bool RegionTable::hasRegionSetFromBB(BasicBlockId bbId) const {
  return this->bbToRegionMap.count(bbId) != 0;
}

const RegionTable::RegionSet &
RegionTable::getRegionSetFromBB(BasicBlockId bbId) const {
  return this->bbToRegionMap.at(bbId);
}} // namespace gem5

