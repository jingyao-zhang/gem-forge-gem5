#ifndef __CPU_GEM_FORGE_REGION_TABLE_HH__
#define __CPU_GEM_FORGE_REGION_TABLE_HH__

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse region information."
#endif

#include "TDGInstruction.pb.h"

#include <unordered_map>
#include <unordered_set>

namespace gem5 {

/**
 * This class contains the region map and bb to region map.
 */
class RegionTable {
public:
  using RegionId = uint64_t;
  static constexpr RegionId REGION_ID_ALL = 0;

  using BasicBlockId = uint64_t;
  using Region = LLVM::TDG::Region;

  using RegionMap = std::unordered_map<RegionId, const Region *>;
  using RegionSet = std::unordered_set<RegionId>;

  RegionTable(const LLVM::TDG::StaticInformation &info);
  ~RegionTable() {}

  const Region &getRegionFromRegionId(const RegionId &regionId) const;
  bool isBBInRegion(BasicBlockId bbId, const RegionId &regionId) const;
  bool hasRegionSetFromBB(BasicBlockId bbId) const;
  const RegionSet &getRegionSetFromBB(BasicBlockId bbId) const;

private:
  RegionMap regions;

  // Reverse map from basic block to regions to speed up the look up.
  std::unordered_map<BasicBlockId, RegionSet> bbToRegionMap;
};

} // namespace gem5

#endif