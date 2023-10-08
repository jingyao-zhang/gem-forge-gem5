#ifndef GEM_FORGE_STREAM_REUSE_INFO_HH
#define GEM_FORGE_STREAM_REUSE_INFO_HH

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace gem5 {

class StreamReuseInfo {
public:
  /**
   * Construct a simple RueseInfo.
   */
  StreamReuseInfo() = default;
  StreamReuseInfo(int64_t _reuseCount) : reuseCount(_reuseCount) {}

  StreamReuseInfo(int64_t _reuseDim, int64_t _reuseDimEnd, int64_t _reuseCount,
                  int64_t _reuseTileSize)
      : reuseDim(_reuseDim), reuseDimEnd(_reuseDimEnd), reuseCount(_reuseCount),
        reuseTileSize(_reuseTileSize) {}

  bool operator==(const StreamReuseInfo &other) const {
    return this->reuseDim == other.reuseDim &&
           this->reuseDimEnd == other.reuseDimEnd &&
           this->reuseCount == other.reuseCount &&
           this->reuseTileSize == other.reuseTileSize;
  }

  bool hasReuse() const { return this->reuseCount != 1; }
  bool isReuseTiled() const { return this->reuseTileSize > 1; }

  int64_t getTotalReuse() const { return this->reuseCount; }

  uint64_t convertBaseToDepElemIdx(uint64_t baseElemIdx) const {
    auto tileOffset = baseElemIdx % reuseTileSize;
    auto tileIdx = baseElemIdx / reuseTileSize;
    auto depElemIdx = tileIdx * reuseTileSize * reuseCount + tileOffset;
    return depElemIdx;
  }

  uint64_t convertDepToBaseElemIdx(uint64_t depElemIdx) const {
    auto tileOffset = depElemIdx % reuseTileSize;
    auto baseElemIdx =
        depElemIdx / (reuseCount * reuseTileSize) * reuseTileSize + tileOffset;
    return baseElemIdx;
  }

  void transformStrideAndTrip(std::vector<int64_t> &strides,
                              std::vector<int64_t> &trips) const {
    assert(reuseDim != InvalidReuseDim);
    assert(reuseDimEnd != InvalidReuseDim);
    assert(strides.at(reuseDim) == 0);
    trips.erase(trips.begin() + reuseDim, trips.begin() + reuseDimEnd);
    strides.erase(strides.begin() + reuseDim, strides.begin() + reuseDimEnd);
  }

private:
  friend std::ostream &operator<<(std::ostream &os,
                                  const StreamReuseInfo &info);

  static constexpr int64_t InvalidReuseDim = -1;
  int64_t reuseDim = InvalidReuseDim;
  int64_t reuseDimEnd = InvalidReuseDim;
  int64_t reuseCount = 1;
  int64_t reuseTileSize = 1;
};

std::ostream &operator<<(std::ostream &os, const StreamReuseInfo &info);

std::string to_string(const StreamReuseInfo &info);

} // namespace gem5

#endif