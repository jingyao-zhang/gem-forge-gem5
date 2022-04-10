#ifndef __CPU_GEM_FORGE_STRAND_SPLIT_INFO_HH__
#define __CPU_GEM_FORGE_STRAND_SPLIT_INFO_HH__

#include "DynStrandId.hh"

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
   *
   * Assume initOffset < interleave.
   *
   * The formula f(Stream) -> Strand:
   * AdjustedStreamElemIdx = StreamElemIdx + initOffset
   * StrandIdx = (AdjustedStreamElemIdx / interleave) % totalStrands
   * StrandElemIdx =
   *   (AdjustedStreamElemIdx / (totalStrands * interleave)) * interleave
   *   + AdjustedStreamElemIdx % interleave
   * If StrandIdx = 0:
   *   StrandElemIdx -= initOffset
   *
   * Reverse: g(Strand) -> Stream
   * If StrandIdx == 0:
   *   StrandElemIdx += initOffset
   * StreamElemIdx =
   *   (StrandElemIdx / interleave) * (totalStrands * interleave)
   *   + StrandIdx * interleave + StrandElemIdx % interleave - initOffset.
   *
   * TripCount of Strand:
   * Let FinalStrandIdx, FinalStrandElemIdx = f(TotalTripCount)
   * If StrandIdx < FinalStrandIdx:
   *   TripCount = (FinalStrandElemIdx / interleave + 1) * interleave
   *   If StrandIdx = 0:
   *     TripCount -= initOffset.
   * If StrandIdx = FinalStrandIdx:
   *   TripCount = FinalStrandElemIdx
   * If StrandIdx > FinalStrandIdx:
   *   TripCount = (FinalStrandElemIdx / interleave) * interleave
   */
  uint64_t initOffset = 0;
  uint64_t interleave = 1;
  uint64_t totalStrands = 1;

  StrandSplitInfo() = default;

  StrandSplitInfo(uint64_t _initOffset, uint64_t _interleave,
                  uint64_t _totalStrands);

  using StreamElemIdx = uint64_t;
  using StrandIdx = DynStrandId::StrandIndex;
  using TripCount = int64_t;

  StrandElemSplitIdx mapStreamToStrand(StreamElemIdx streamElemIdx) const;
  StreamElemIdx mapStrandToStream(StrandElemSplitIdx strandElemIdx) const;
  TripCount getStrandTripCount(TripCount streamTripCount,
                               StrandIdx strandIdx) const;
};

#endif