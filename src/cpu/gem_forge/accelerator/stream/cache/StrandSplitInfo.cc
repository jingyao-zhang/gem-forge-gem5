#include "StrandSplitInfo.hh"

#include "base/trace.hh"

#include "debug/MLCRubyStrandSplit.hh"

#include <cassert>

StrandSplitInfo::StrandSplitInfo(int64_t _initOffset, int64_t _interleave,
                                 int64_t _tailInterleave, int64_t _totalStrands)
    : initOffset(_initOffset), interleave(_interleave),
      tailInterleave(_tailInterleave), totalStrands(_totalStrands) {
  assert(initOffset == 0 && "InitOffset must be zero for now.");
  assert(initOffset < interleave && "InitOffset >= Interleave.");
  assert(tailInterleave < interleave * totalStrands &&
         "TailInterleave >= Interleave * totalStrands.");
}

StrandElemSplitIdx
StrandSplitInfo::mapStreamToStrand(StreamElemIdx streamElemIdx) const {

  auto elemPerRound = this->getElemPerRound();
  auto roundIdx = streamElemIdx / elemPerRound;
  auto roundElemIdx = streamElemIdx % elemPerRound;

  auto strandIdx = roundElemIdx / interleave;
  auto strandRoundElem = this->getElemPerStrandRound(strandIdx);
  auto strandElemIdx = roundIdx * strandRoundElem + roundElemIdx % interleave;

  return StrandElemSplitIdx(strandIdx, strandElemIdx);
}

StrandSplitInfo::StreamElemIdx
StrandSplitInfo::mapStrandToStream(StrandElemSplitIdx strandElemSplit) const {
  auto strandIdx = strandElemSplit.strandIdx;
  auto strandElemIdx = strandElemSplit.elemIdx;
  auto elemPerRound = this->getElemPerRound();
  auto strandRoundElem = this->getElemPerStrandRound(strandIdx);
  if (strandRoundElem == 0) {
    // This strand is empty.
    return 0;
  }
  auto roundIdx = strandElemIdx / strandRoundElem;
  auto roundElemIdx = strandIdx * interleave + strandElemIdx % strandRoundElem;
  auto streamElemIdx = roundIdx * elemPerRound + roundElemIdx;
  return streamElemIdx;
}

StrandSplitInfo::TripCount
StrandSplitInfo::getStrandTripCount(TripCount streamTripCount,
                                    StrandIdx strandIdx) const {
  auto elemPerRound = this->getElemPerRound();
  auto strandRoundElem = this->getElemPerStrandRound(strandIdx);
  auto strandElemSplit = this->mapStreamToStrand(streamTripCount);
  auto finalStrandIdx = strandElemSplit.strandIdx;
  auto finalStrandElemIdx = strandElemSplit.elemIdx;
  auto finalRoundIdx = streamTripCount / elemPerRound;

  auto tripCount = finalStrandElemIdx;
  if (strandIdx < finalStrandIdx) {
    tripCount = (finalRoundIdx + 1) * strandRoundElem;
  } else if (strandIdx > finalStrandIdx) {
    tripCount = finalRoundIdx * strandRoundElem;
  }
  DPRINTF(MLCRubyStrandSplit,
          "StreamTrip %ld StrandIdx %d FinalStrandIdx %d "
          "FinalStrandElemIdx %lu TripCount %lu.\n",
          streamTripCount, strandIdx, finalStrandIdx, finalStrandElemIdx,
          tripCount);
  return tripCount;
}