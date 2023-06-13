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
   *
   * This InitOffset is for case when StreamStartAddr is not aligned to the
   * bank.
   * ! For now we assume InitOffset is always zero, as it's unused.
   *
   * | Strand0-Intrlv0 | Strand1-Intrlv0 | Strand0-Intrlv1 | Strand1-Intrlv1 |
   * |-------------------------------| T |-------------------------------| T |
   *
   * Here T is used to handle some tailing missing elements.
   * For example: the pattern is 0:1:510:512:510:26244:8
   * With interleave = 510x8 = 4080, each strand handles 8 rows.
   * However, the last strand only handles 6 rows,
   * with tailInterleave = 510*2 = 1020.
   *
   * ElemPerRound = interleave * totalStrands - tailInterleave
   *
   * ElemPerStrandRound(StrandIdx) =
   *      min(ElemPerRound, (StrandIdx + 1) * interleave)
   *    - min(ElemPerRound, StrandIdx * interleave)
   *
   * The formula f(Stream) -> Strand:
   * RoundIdx = StreamElemIdx / ElemPerRound
   * RoundElemIdx = StreamElemIdx % ElemPerRound
   *
   * StrandIdx = RoundElemIdx / interleave
   * StrandRoundElem = ElemPerStrandRound(StrandIdx)
   * StrandElemIdx =
   *      RoundIdx * StrandRoundElem
   *    + RoundElemIdx % interleave
   *
   * Reverse: g(Strand) -> Stream
   * StrandRoundElem = ElemPerStrandRound(StrandIdx)
   * if StrandRoundElem == 0:
   *    RoundIdx = 0
   *    RoundElemIdx = 0
   * else:
   *    RoundIdx = StrandElemIdx / StrandRoundElem
   *    RoundElemIdx = StrandIdx * interleave
   *       + StrandElemIdx % StrandRoundElem
   *
   * StreamElemIdx =
   *      RoundIdx * ElemPerRound
   *    + RoundElemIdx
   *
   * TripCount of Strand:
   * Let FinalStrandIdx, FinalRoundIdx, FinalStrandElemIdx = f(TotalTripCount)
   * StrandRoundElem = ElemPerStrandRound(StrandIdx)
   * If StrandIdx < FinalStrandIdx:
   *   TripCount = (FinalRoundIdx + 1) * StrandRoundElem
   * If StrandIdx = FinalStrandIdx:
   *   TripCount = FinalStrandElemIdx
   * If StrandIdx > FinalStrandIdx:
   *   TripCount = FinalRoundIdx * StrandRoundElem
   *
   * Second implementation: split some dimension.
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

  bool isSplitByDim() const { return splitByDim; }
  bool isSplitByElem() const { return splitByElem; }

private:
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
  TripCount getStrandTripCountByDim(TripCount streamTripCount,
                                    StrandIdx strandIdx) const;
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