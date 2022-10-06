#include "StrandSplitInfo.hh"

#include "base/logging.hh"
#include "base/trace.hh"

#include "debug/MLCRubyStrandSplit.hh"

#include <cassert>

StrandSplitInfo::StrandSplitInfo(int64_t _interleave, int64_t _tailInterleave,
                                 int64_t _totalStrands)
    : interleave(_interleave), tailInterleave(_tailInterleave),
      totalStrands(_totalStrands) {
  assert(tailInterleave < interleave * totalStrands &&
         "TailInterleave >= Interleave * totalStrands.");

  // We also try to set the parameters for new implementation.
  this->innerTrip = 1;
  this->splitTrip = interleave * totalStrands - tailInterleave;
  this->splitTripPerStrand = interleave;
}

StrandSplitInfo::StrandSplitInfo(int64_t _innerTrip, int64_t _splitTrip,
                                 int64_t _splitTripPerStrand,
                                 int64_t _totalStrands)
    : totalStrands(_totalStrands), splitByDim(true), innerTrip(_innerTrip),
      splitTrip(_splitTrip), splitTripPerStrand(_splitTripPerStrand) {}

StrandSplitInfo::TripCount
StrandSplitInfo::getSplitDimTotalTripForStrand(StrandIdx strandIdx) const {

  auto a = totalStrands * splitTripPerStrand;
  auto finalStrandIdx = (splitTrip % a) / splitTripPerStrand;
  auto x = (splitTrip / a) * splitTripPerStrand;
  auto y = x + splitTripPerStrand;
  auto z =
      splitTrip - finalStrandIdx * y - (totalStrands - finalStrandIdx - 1) * x;

  DPRINTF(MLCRubyStrandSplit,
          "[SplitDimTrip] InnerTrip %ld SplitTrip %ld SplitTripPerStrand %ld "
          "Strand %ld FinalStrandIdx %ld SplitStrandTrip %ld-%ld-%ld.\n",
          innerTrip, splitTrip, splitTripPerStrand, strandIdx, finalStrandIdx,
          y, z, x);

  assert(z >= 0 && x >= 0 && y >= 0);

  if (strandIdx < finalStrandIdx) {
    return y;
  } else if (strandIdx > finalStrandIdx) {
    return x;
  } else {
    return z;
  }
}

StrandElemSplitIdx
StrandSplitInfo::mapStreamToStrandImpl(StreamElemIdx streamElemIdx) const {

  auto innerIdx = streamElemIdx % innerTrip;
  auto splitIdx = (streamElemIdx / innerTrip) % splitTrip;
  auto outerIdx = (streamElemIdx / innerTrip / splitTrip);

  auto x = splitIdx % splitTripPerStrand;
  auto y = splitIdx / splitTripPerStrand;
  auto z = y / totalStrands;

  auto strandIdx = (splitIdx / splitTripPerStrand) % totalStrands;
  auto splitDimStrandTrip = this->getSplitDimTotalTripForStrand(strandIdx);

  // We should never be mapped to empty strand.
  if (splitDimStrandTrip == 0) {
    panic("StreamElemIdx %lu mapped to EmptyStrand %lu.", streamElemIdx,
          strandIdx);
  }

  auto strandElemIdx = innerIdx + innerTrip * (z * splitTripPerStrand + x) +
                       innerTrip * splitDimStrandTrip * outerIdx;
  return StrandElemSplitIdx(strandIdx, strandElemIdx);
}

StrandElemSplitIdx
StrandSplitInfo::mapStreamToStrand(StreamElemIdx streamElemIdx) const {

  auto newRet = this->mapStreamToStrandImpl(streamElemIdx);

  if (splitByDim) {
    return newRet;
  }

  auto elemPerRound = this->getElemPerRound();
  auto roundIdx = streamElemIdx / elemPerRound;
  auto roundElemIdx = streamElemIdx % elemPerRound;

  auto strandIdx = roundElemIdx / interleave;
  auto strandRoundElem = this->getElemPerStrandRound(strandIdx);
  auto strandElemIdx = roundIdx * strandRoundElem + roundElemIdx % interleave;

  if (strandIdx != newRet.strandIdx || strandElemIdx != newRet.elemIdx) {
    panic("Mismatch StreamElemIdx %ld Old %ld-%ld New %ld-%ld Intrlv %ld "
          "TailIntrlv %ld.",
          streamElemIdx, strandIdx, strandElemIdx, newRet.strandIdx,
          newRet.elemIdx, this->interleave, this->tailInterleave);
  }

  return StrandElemSplitIdx(strandIdx, strandElemIdx);
}

StrandSplitInfo::StreamElemIdx StrandSplitInfo::mapStrandToStreamImpl(
    StrandElemSplitIdx strandElemSplit) const {
  auto strandIdx = strandElemSplit.strandIdx;
  auto strandElemIdx = strandElemSplit.elemIdx;

  auto strandSplitTrip = this->getSplitDimTotalTripForStrand(strandIdx);

  auto innerIdx = strandElemIdx % innerTrip;
  auto strandSplitIdx = (strandElemIdx / innerTrip) % strandSplitTrip;
  auto outerIdx = strandElemIdx / innerTrip / strandSplitTrip;

  auto splitRound = strandSplitIdx / splitTripPerStrand;
  auto splitIdx = splitRound * totalStrands * splitTripPerStrand +
                  strandIdx * splitTripPerStrand +
                  strandSplitIdx % splitTripPerStrand;

  return innerIdx + innerTrip * splitIdx + innerTrip * splitTrip * outerIdx;
}

StrandSplitInfo::StreamElemIdx
StrandSplitInfo::mapStrandToStream(StrandElemSplitIdx strandElemSplit) const {
  auto strandIdx = strandElemSplit.strandIdx;
  auto strandElemIdx = strandElemSplit.elemIdx;

  auto newRet = this->mapStrandToStreamImpl(strandElemSplit);

  if (splitByDim) {
    return newRet;
  }

  auto elemPerRound = this->getElemPerRound();
  auto strandRoundElem = this->getElemPerStrandRound(strandIdx);
  if (strandRoundElem == 0) {
    // This strand is empty.
    return 0;
  }
  auto roundIdx = strandElemIdx / strandRoundElem;
  auto roundElemIdx = strandIdx * interleave + strandElemIdx % strandRoundElem;
  auto streamElemIdx = roundIdx * elemPerRound + roundElemIdx;

  if (streamElemIdx != newRet) {
    panic("Mismatch StreamElemIdx %ld-%ld Old %ld New %ld Intrlv %ld "
          "TailIntrlv %ld.",
          strandIdx, strandElemIdx, streamElemIdx, newRet, this->interleave,
          this->tailInterleave);
  }

  return streamElemIdx;
}

StrandSplitInfo::TripCount
StrandSplitInfo::getStrandTripCountImpl(TripCount streamTripCount,
                                        StrandIdx strandIdx) const {

  if ((streamTripCount % (innerTrip * splitTrip)) != 0) {
    panic("StreamTrip %ld InnerTrip %ld SplitTrip %ld.", streamTripCount,
          innerTrip, splitTrip);
  }
  auto outerTrip = streamTripCount / (innerTrip * splitTrip);

  auto strandSplitTrip = this->getSplitDimTotalTripForStrand(strandIdx);

  return innerTrip * strandSplitTrip * outerTrip;
}

StrandSplitInfo::TripCount
StrandSplitInfo::getStrandTripCount(TripCount streamTripCount,
                                    StrandIdx strandIdx) const {

  auto newRet = this->getStrandTripCountImpl(streamTripCount, strandIdx);
  if (this->splitByDim) {
    return newRet;
  }

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

  if (newRet != tripCount) {
    panic(
        "TripCountMismatch streamTripCount %ld StrandIdx %ld Old %ld New %ld.",
        streamTripCount, strandIdx, tripCount, newRet);
  }
  return tripCount;
}