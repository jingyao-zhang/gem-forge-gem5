#ifndef __CPU_GEM_FORGE_STRAND_SPLIT_INFO_HH__
#define __CPU_GEM_FORGE_STRAND_SPLIT_INFO_HH__

#include "DynStrandId.hh"

#include <cassert>
#include <vector>

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

  StrandSplitInfo(int64_t _innerTrip, int64_t _splitTrip,
                  int64_t _splitTripPerStrand, int64_t _totalStrands);
  StrandSplitInfo(const std::vector<StreamElemIdx> &_streamElemSplits,
                  int64_t _totalStrands);

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
    return this->innerTrip * this->splitTripPerStrand;
  }
  int64_t getInnerTrip() const {
    assert(this->splitByDim);
    return this->innerTrip;
  }
  void setInnerTrip(int64_t innerTrip) {
    assert(this->splitByDim);
    this->innerTrip = innerTrip;
  }
  int64_t getTailInterleave() const {
    assert(this->splitByDim);
    return this->splitTrip % (this->splitTripPerStrand * this->totalStrands);
  }

  bool isSplitByDim() const { return splitByDim; }
  bool isSplitByElem() const { return splitByElem; }

private:
  int64_t interleave = 1;
  int64_t tailInterleave = 0;
  int64_t totalStrands = 1;

  bool splitByDim = false;
  int64_t innerTrip = 1;
  int64_t splitTrip = 1;
  int64_t splitTripPerStrand = 1;

  TripCount getElemPerRound() const {
    return interleave * totalStrands - tailInterleave;
  }
  TripCount getElemPerStrandRound(StrandIdx strandIdx) const {
    auto elemPerRound = this->getElemPerRound();
    return std::min(elemPerRound, (strandIdx + 1) * interleave) -
           std::min(elemPerRound, strandIdx * interleave);
  }

  /**
   * These are the new implementation to splitByDim.
   */
  StrandElemSplitIdx mapStreamToStrandByDim(StreamElemIdx streamElemIdx) const;
  StreamElemIdx mapStrandToStreamByDim(StrandElemSplitIdx strandElemIdx) const;
  TripCount getStrandTripCountByDim(TripCount streamTripCount,
                                    StrandIdx strandIdx) const;
  std::vector<StrandElemSplitIdx>
  mapStreamToPrevStrandByDim(StreamElemIdx streamElemIdx) const;
  TripCount getSplitDimTotalTripForStrand(StrandIdx strandIdx) const;

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

#endif