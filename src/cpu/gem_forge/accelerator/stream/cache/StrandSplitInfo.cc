#include "StrandSplitInfo.hh"

#include <cassert>

StrandSplitInfo::StrandSplitInfo(uint64_t _initOffset, uint64_t _interleave,
                                 uint64_t _totalStrands)
    : initOffset(_initOffset), interleave(_interleave),
      totalStrands(_totalStrands) {
  assert(initOffset < interleave && "InitOffset >= Interleave.");
}

StrandElemSplitIdx
StrandSplitInfo::mapStreamToStrand(StreamElemIdx streamElemIdx) const {
  auto adjStreamElemIdx = streamElemIdx + this->initOffset;
  auto strandIdx = (adjStreamElemIdx / this->interleave) % totalStrands;
  auto strandElemIdx =
      (adjStreamElemIdx / (this->totalStrands * this->interleave)) *
          this->interleave +
      adjStreamElemIdx % interleave;
  if (strandIdx == 0) {
    assert(strandElemIdx >= initOffset &&
           "First strand should start with initOffset.");
    strandElemIdx -= this->initOffset;
  }
  return StrandElemSplitIdx(strandIdx, strandElemIdx);
}

StrandSplitInfo::StreamElemIdx
StrandSplitInfo::mapStrandToStream(StrandElemSplitIdx strandElemSplit) const {
  auto strandIdx = strandElemSplit.strandIdx;
  auto strandElemIdx = strandElemSplit.elemIdx;
  if (strandIdx == 0) {
    strandElemIdx += this->initOffset;
  }
  auto streamElemIdx = (strandElemIdx / this->interleave) *
                           (this->totalStrands * this->interleave) +
                       strandIdx * this->interleave +
                       strandElemIdx % this->interleave;
  assert(streamElemIdx >= this->initOffset && "StreamElemIdx < InitOffset.");
  streamElemIdx -= this->initOffset;
  return streamElemIdx;
}

StrandSplitInfo::TripCount
StrandSplitInfo::getStrandTripCount(TripCount streamTripCount,
                                    StrandIdx strandIdx) const {
  auto strandElemSplit = this->mapStreamToStrand(streamTripCount);
  auto finalStrandIdx = strandElemSplit.strandIdx;
  auto finalStrandElemIdx = strandElemSplit.elemIdx;
  auto tripCount = finalStrandElemIdx;
  if (strandIdx < finalStrandIdx) {
    tripCount = (finalStrandElemIdx / this->interleave + 1) * this->interleave;
    if (strandIdx == 0) {
      assert(tripCount > this->initOffset && "TripCount <= InitOffset.");
      tripCount -= this->initOffset;
    }
  } else if (strandIdx > finalStrandIdx) {
    tripCount = (finalStrandElemIdx / this->interleave) * this->interleave;
  }
  return tripCount;
}