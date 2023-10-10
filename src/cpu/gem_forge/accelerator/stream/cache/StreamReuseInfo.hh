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
  StreamReuseInfo() { this->addTile(0, 0, 1, 1); }
  StreamReuseInfo(int64_t _reuseCount) { this->addTile(0, 0, _reuseCount, 1); }
  StreamReuseInfo(int64_t _reuseDim, int64_t _reuseDimEnd, int64_t _reuseCount,
                  int64_t _reuseTileSize) {
    this->addTile(_reuseDim, _reuseDimEnd, _reuseCount, _reuseTileSize);
  }

  void addTile(int64_t _reuseDim, int64_t _reuseDimEnd, int64_t _reuseCount,
               int64_t _reuseTileSize);

  bool operator==(const StreamReuseInfo &other) const {
    if (this->tiles.size() != other.tiles.size()) {
      return false;
    }
    for (int i = 0; i < this->tiles.size(); ++i) {
      if (this->tiles[i] != other.tiles[i]) {
        return false;
      }
    }
    return true;
  }

  bool hasReuse() const {
    // Should be sufficient to check the first one.
    return this->tiles.size() > 1 || this->tiles.front().hasReuse();
  }
  bool isReuseTiled() const {
    // Only tiled reuse need more than one ReusedTile.
    return this->tiles.size() > 1 || this->tiles.front().isReuseTiled();
  }
  bool isInnerLoopReuse() const {
    // This is reused by some inner loop so without valid ReuseDim.
    return this->tiles.size() == 1 &&
           this->tiles.front().reuseDim == this->tiles.front().reuseDimEnd;
  }
  int64_t getTotalReuse() const { return this->totalReuseCount; }

  uint64_t convertBaseToDepElemIdx(uint64_t baseElemIdx) const;
  uint64_t convertDepToBaseElemIdx(uint64_t depElemIdx) const;
  void transformStrideAndTrip(std::vector<int64_t> &strides,
                              std::vector<int64_t> &trips) const;

  /**
   * Helper function to merge NormalReuse and InnerLoopReuse.
   */
  StreamReuseInfo mergeInnerLoopReuse(const StreamReuseInfo &reuseInfo) const;

private:
  friend std::ostream &operator<<(std::ostream &os,
                                  const StreamReuseInfo &info);

  static constexpr int64_t InvalidReuseDim = -1;
  struct ReusedTile {
    int64_t reuseDim = InvalidReuseDim;
    int64_t reuseDimEnd = InvalidReuseDim;
    int64_t reuseCount = 1;
    int64_t reuseTileSize = 1;

    ReusedTile() = default;
    ReusedTile(int64_t _reuseCount) : reuseCount(_reuseCount) {}
    ReusedTile(int64_t _reuseDim, int64_t _reuseDimEnd, int64_t _reuseCount,
               int64_t _reuseTileSize)
        : reuseDim(_reuseDim), reuseDimEnd(_reuseDimEnd),
          reuseCount(_reuseCount), reuseTileSize(_reuseTileSize) {}
    bool operator==(const ReusedTile &other) const {
      return this->reuseDim == other.reuseDim &&
             this->reuseDimEnd == other.reuseDimEnd &&
             this->reuseCount == other.reuseCount &&
             this->reuseTileSize == other.reuseTileSize;
    }
    bool operator!=(const ReusedTile &other) const {
      return !this->operator==(other);
    }

    bool hasReuse() const { return reuseCount != 1; }
    bool isReuseTiled() const { return reuseTileSize > 1; }

    int64_t getTotalReuse() const { return reuseCount; }

    uint64_t convertBaseToDepElemIdx(uint64_t baseElemIdx) const;
    uint64_t convertDepToBaseElemIdx(uint64_t depElemIdx) const;
    void transformStrideAndTrip(std::vector<int64_t> &strides,
                                std::vector<int64_t> &trips) const;
  };
  std::vector<ReusedTile> tiles;
  int64_t totalReuseCount;

  void updateTotalReuseCount() {
    int64_t totalReuseCount = 1;
    for (const auto &tile : this->tiles) {
      totalReuseCount *= tile.getTotalReuse();
    }
    this->totalReuseCount = totalReuseCount;
  }
};

std::ostream &operator<<(std::ostream &os, const StreamReuseInfo &info);

std::string to_string(const StreamReuseInfo &info);

} // namespace gem5

#endif