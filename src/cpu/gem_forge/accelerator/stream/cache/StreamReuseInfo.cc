#include "StreamReuseInfo.hh"

#include <sstream>

namespace gem5 {

std::ostream &operator<<(std::ostream &os, const StreamReuseInfo &info) {
  for (int i = 0; i < info.tiles.size(); ++i) {
    if (i > 0) {
      os << ',';
    }
    const auto &tile = info.tiles[i];
    os << tile.reuseTileSize << 'x' << tile.reuseCount << '@' << tile.reuseDim
       << '-' << tile.reuseDimEnd;
  }
  return os;
}

std::string to_string(const StreamReuseInfo &info) {
  std::ostringstream ss;
  ss << info;
  return ss.str();
}

void StreamReuseInfo::addTile(int64_t _reuseDim, int64_t _reuseDimEnd,
                              int64_t _reuseCount, int64_t _reuseTileSize) {
  tiles.emplace_back(_reuseDim, _reuseDimEnd, _reuseCount, _reuseTileSize);
  this->updateTotalReuseCount();
}

uint64_t StreamReuseInfo::ReusedTile::convertBaseToDepElemIdx(
    uint64_t baseElemIdx) const {
  auto tileOffset = baseElemIdx % reuseTileSize;
  auto tileIdx = baseElemIdx / reuseTileSize;
  auto depElemIdx = tileIdx * reuseTileSize * reuseCount + tileOffset;
  return depElemIdx;
}

uint64_t StreamReuseInfo::ReusedTile::convertDepToBaseElemIdx(
    uint64_t depElemIdx) const {
  auto tileOffset = depElemIdx % reuseTileSize;
  auto baseElemIdx =
      depElemIdx / (reuseCount * reuseTileSize) * reuseTileSize + tileOffset;
  return baseElemIdx;
}

uint64_t StreamReuseInfo::convertBaseToDepElemIdx(uint64_t baseElemIdx) const {
  auto curElemIdx = baseElemIdx;
  // Need to process in reverse order.
  for (auto iter = this->tiles.rbegin(), end = this->tiles.rend(); iter != end;
       ++iter) {
    const auto &tile = *iter;
    curElemIdx = tile.convertBaseToDepElemIdx(curElemIdx);
  }
  return curElemIdx;
}

uint64_t StreamReuseInfo::convertDepToBaseElemIdx(uint64_t depElemIdx) const {
  auto curElemIdx = depElemIdx;
  for (const auto &tile : this->tiles) {
    curElemIdx = tile.convertDepToBaseElemIdx(curElemIdx);
  }
  return curElemIdx;
}

void StreamReuseInfo::ReusedTile::transformStrideAndTrip(
    std::vector<int64_t> &strides, std::vector<int64_t> &trips) const {
  assert(reuseDim != InvalidReuseDim);
  assert(reuseDimEnd != InvalidReuseDim);
  assert(strides.at(reuseDim) == 0);
  trips.erase(trips.begin() + reuseDim, trips.begin() + reuseDimEnd);
  strides.erase(strides.begin() + reuseDim, strides.begin() + reuseDimEnd);
}

void StreamReuseInfo::transformStrideAndTrip(
    std::vector<int64_t> &strides, std::vector<int64_t> &trips) const {
  // Apply them in reverse order from outer-most reuse.
  for (auto iter = this->tiles.rbegin(), end = this->tiles.rend(); iter != end;
       ++iter) {
    iter->transformStrideAndTrip(strides, trips);
  }
}

StreamReuseInfo
StreamReuseInfo::mergeInnerLoopReuse(const StreamReuseInfo &reuseInfo) const {
  assert(reuseInfo.isInnerLoopReuse() && "This is not InnerLoopReuse.");

  if (!reuseInfo.hasReuse()) {
    // No need to merge, simply return myself.
    return *this;
  }

  auto newInfo = reuseInfo;
  for (const auto &tile : this->tiles) {
    newInfo.addTile(tile.reuseDim, tile.reuseDimEnd, tile.reuseCount,
                    tile.reuseTileSize);
  }
  return newInfo;
}

} // namespace gem5