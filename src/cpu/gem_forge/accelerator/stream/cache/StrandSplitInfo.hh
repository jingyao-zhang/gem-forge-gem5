#ifndef __CPU_GEM_FORGE_STRAND_SPLIT_INFO_HH__
#define __CPU_GEM_FORGE_STRAND_SPLIT_INFO_HH__

#include "DynStrandId.hh"

#include <cassert>
#include <vector>

namespace gem5 {

struct StrandElemSplitIdx {
  /**
   * Represent an element within one strand.
   */
  using StrandIdx = DynStrandId::StrandIndex;
  using ElementIdx = uint64_t;
  const StrandIdx strandIdx;
  const ElementIdx elemIdx;
  StrandElemSplitIdx(StrandIdx _strandIdx, ElementIdx _elemIdx)
      : strandIdx(_strandIdx), elemIdx(_elemIdx) {}
};

class StrandSplitInfo {
public:
  /**
   * Represents the mapping between Stream and Strand.
   *
   * So far we assume the canonical loop structure with N levels,
   * with trip Ti for each level, the original StreamIdx form a N-dim
   * hyper-rectangle. Define Ai as the accumulated trips:
   *
   * Ai = T0 x T1 x ... T_{i-1} with A0 = 1
   *
   * Given a StreamIdx, we can find its coordinate within this hyper-rectangle:
   *
   *  Xi = (StreamIdx / Ai) % Ti
   *
   * We can reuse AffinePattern X to represent this hyper-rectangle, with
   * strides all set to 1.
   *
   * Now, for arbitrary dimension i, we can split this 1-Dim into:
   *
   *  SplitCnt (SC): How many split happens at this dimension.
   *  SplitIntrlv (SI): Split interleave at this dimension.
   *
   * To translate from Stream to Strand:
   *
   * | Strand0-Intrlv0 | Strand1-Intrlv0 | Strand0-Intrlv1 | Strand1-Intrlv1 |
   * |------------------------------Ti-------------------------------|       |
   *
   * StrandId = (Xi / SI) % SC
   * StrandIdx = Xi * Bi (for non-split i-dim) +
   *             ((Xi / SI / SC) * SI + Xi % SI) * Bi (for splitted i-dim)
   * where Bi is the adjusted accumulated trip for that strand.
   *
   * After split, the trip at that dimension for a strand:
   *
   * FinalStrandId = (Ti / SI) % SC
   *
   * If StrandId < FinalStrandId:
   *   Ri = (Ti / SI / SC + 1) * SI
   * If StrandIdx = FinalStrandIdx:
   *   Ri = (Ti / SI / SC) * SI + (Ti % SI)
   * If StrandIdx > FinalStrandIdx:
   *   Ri = (Ti / SI / SC) * SI
   *
   * And Bi is accumulated trip on Ri.
   */

  StrandSplitInfo() = default;

public:
  using StreamElemIdx = uint64_t;
  using StrandIdx = DynStrandId::StrandIndex;
  using TripCount = int64_t;
  using IntVecT = std::vector<int64_t>;

  // StrandSplitInfo(int64_t _innerTrip, int64_t _splitTrip,
  //                 int64_t _splitTripPerStrand, int64_t _totalStrands);
  StrandSplitInfo(const std::vector<StreamElemIdx> &_streamElemSplits,
                  int64_t _totalStrands);

  struct SplitDim {
    int dim = 0;
    int cnt = 1;
    int intrlv = 1;
    SplitDim(int _dim, int _cnt, int _intrlv)
        : dim(_dim), cnt(_cnt), intrlv(_intrlv) {}
  };
  StrandSplitInfo(const IntVecT &trips, const std::vector<SplitDim> &splitDims);

  int64_t getTotalStrands() const { return this->totalStrands; }

  StrandElemSplitIdx mapStreamToStrand(StreamElemIdx streamElemIdx) const;
  StreamElemIdx mapStrandToStream(StrandElemSplitIdx strandElemIdx) const;
  TripCount getStrandTripCount(TripCount streamTripCount,
                               StrandIdx strandIdx) const;

  TripCount getStrandTripCountByDim(StrandIdx strandIdx,
                                    int untilDim = -1) const;

  /**
   * Get all StrandElemSplitIdxes that would happen before the given
   * StreamElemIdx. Used to check the serialized progress of strands.
   */
  std::vector<StrandElemSplitIdx>
  mapStreamToPrevStrand(StreamElemIdx streamElemIdx) const;

  int64_t getInterleave() const {
    assert(this->splitByDim);
    assert(this->dimensions.size() == 1);
    return this->dimensions.front().splitIntrlv;
  }

  StrandIdx getStrandIdAtDim(StrandIdx strandIdx, int dimension) const {
    assert(this->splitByDim);
    const auto &dim = this->dimensions.at(dimension);
    return (strandIdx / dim.accSplitCnt) % dim.splitCnt;
  }

  /**
   * Check if the split is homogenious to another split.
   * 1. We have dimensions equal to split after adjust by loopDiff.
   * 2. All dimensions of split matches with ours (same trip, splitCnt,
   * splitIntrlv).
   */
  bool isHomogeniousSplitTo(const StrandSplitInfo &split, int loopDiff) const {
    if (loopDiff < 0) {
      return split.isHomogeniousSplitTo(*this, -loopDiff);
    }
    if (!this->isSplitByDim() || !split.isSplitByDim() ||
        this->dimensions.size() + loopDiff != split.dimensions.size()) {
      return false;
    }
    for (int i = 0; i < this->dimensions.size(); ++i) {
      // Compare in reverse order.
      const auto &dim1 = this->dimensions.at(i);
      const auto &dim2 = split.dimensions.at(i + loopDiff);
      if (dim1.trip != dim2.trip || dim1.splitCnt != dim2.splitCnt ||
          dim1.splitIntrlv != dim2.splitIntrlv) {
        return false;
      }
    }
    return true;
  }

  /**
   * Check if both strand are homogenious mapped together.
   */
  bool isHomogeniousMatch(const StrandSplitInfo &split, int loopDiff,
                          StrandIdx myStrandIdx, StrandIdx strandIdx) const {
    if (loopDiff < 0) {
      return split.isHomogeniousMatch(*this, -loopDiff, strandIdx, myStrandIdx);
    }
    if (!this->isHomogeniousSplitTo(split, loopDiff)) {
      return false;
    }
    for (int i = 0; i < this->dimensions.size(); ++i) {
      auto myStrandId = this->getStrandIdAtDim(myStrandIdx, i);
      auto strandId = split.getStrandIdAtDim(strandIdx, i + loopDiff);
      if (myStrandId != strandId) {
        return false;
      }
    }
    return true;
  }

  bool isSplitByDim() const { return splitByDim; }
  bool isSplitByElem() const { return splitByElem; }

// private:
  int64_t totalStrands = 1;

  bool splitByDim = false;
  struct Dimension {
    int64_t trip;
    int64_t splitCnt;
    int64_t splitIntrlv;
    int64_t lastStrandId;
    int64_t accTrip = 1;
    int64_t accSplitCnt = 1;
    // Non-split dimension.
    Dimension(int64_t _trip)
        : trip(_trip), splitCnt(1), splitIntrlv(1), lastStrandId(0) {}
    // Split dimension.
    Dimension(int64_t _trip, int64_t _splitCnt, int64_t _splitIntrlv)
        : trip(_trip), splitCnt(_splitCnt), splitIntrlv(_splitIntrlv) {
      this->lastStrandId = this->getStrandId(this->trip);
    }
    // Set the split cnt and intrlv.
    void setSplitIntrlvCnt(int64_t splitCnt, int64_t splitIntrlv) {
      this->splitCnt = splitCnt;
      this->splitIntrlv = splitIntrlv;
      this->lastStrandId = this->getStrandId(this->trip);
    }
    // Get the strand id.
    int64_t getStrandId(int64_t streamOffset) const;
    // Get the strand offset.
    int64_t getStrandOffset(int64_t streamOffset) const;
    // Get the stream offset.
    int64_t getStreamOffset(int64_t strandId, int64_t strandOffset) const;
    // Get the strand trip.
    int64_t getStrandTrip(int64_t strandId) const;
  };
  std::vector<Dimension> dimensions;

  /**
   * These are the new implementation to splitByDim.
   */
  StrandElemSplitIdx mapStreamToStrandByDim(StreamElemIdx streamElemIdx) const;
  StreamElemIdx mapStrandToStreamByDim(StrandElemSplitIdx strandElemIdx) const;
  std::vector<StrandElemSplitIdx>
  mapStreamToPrevStrandByDim(StreamElemIdx streamElemIdx) const;

  struct MapToPrevStrandByDimContext {
    int dim;
    IntVecT targetStrandIds;
    IntVecT strandIds;
    IntVecT strandOffsets;
    bool lessThanTargetStrandId;
    std::vector<StrandElemSplitIdx> ret;
  };
  void
  mapStreamToPrevStrandByDimImpl(MapToPrevStrandByDimContext &context) const;

  /**
   * This is a special implementation for short streams used in graph workloads.
   * Each strand takes cares of one bank.
   * We explicitly record the range of each strand here.
   */
  bool splitByElem = false;
  std::vector<StreamElemIdx> streamElemSplits;

  StrandElemSplitIdx mapStreamToStrandByElem(StreamElemIdx streamElemIdx) const;
  StreamElemIdx mapStrandToStreamByElem(StrandElemSplitIdx strandElemIdx) const;
  TripCount getStrandTripCountByElem(TripCount streamTripCount,
                                     StrandIdx strandIdx) const;
  std::vector<StrandElemSplitIdx>
  mapStreamToPrevStrandByElem(StreamElemIdx streamElemIdx) const;
};

} // namespace gem5

#endif