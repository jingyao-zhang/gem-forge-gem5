#include "StrandSplitInfo.hh"

#include "base/logging.hh"
#include "base/trace.hh"

#include "debug/MLCRubyStrandSplit.hh"

#include <cassert>

StrandSplitInfo::StrandSplitInfo(int64_t _innerTrip, int64_t _splitTrip,
                                 int64_t _splitTripPerStrand,
                                 int64_t _totalStrands)
    : totalStrands(_totalStrands), splitByDim(true), innerTrip(_innerTrip),
      splitTrip(_splitTrip), splitTripPerStrand(_splitTripPerStrand) {}

StrandSplitInfo::StrandSplitInfo(
    const std::vector<StreamElemIdx> &_streamElemSplits, int64_t _totalStrands)
    : totalStrands(_totalStrands), splitByElem(true),
      streamElemSplits(_streamElemSplits) {}

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
StrandSplitInfo::mapStreamToStrand(StreamElemIdx streamElemIdx) const {

  if (this->splitByElem) {
    return this->mapStreamToStrandByElem(streamElemIdx);
  }

  auto newRet = this->mapStreamToStrandByDim(streamElemIdx);

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

std::vector<StrandElemSplitIdx>
StrandSplitInfo::mapStreamToPrevStrand(StreamElemIdx streamElemIdx) const {

  if (this->splitByElem) {
    return this->mapStreamToPrevStrandByElem(streamElemIdx);
  }

  if (this->splitByDim) {
    return this->mapStreamToPrevStrandByDim(streamElemIdx);
  }

  // Should be no split.
  assert(this->interleave == 1);
  assert(this->tailInterleave == 0);
  assert(this->totalStrands == 1);

  StrandIdx strandIdx = 0;
  StreamElemIdx strandElemIdx = streamElemIdx;

  std::vector<StrandElemSplitIdx> ret;
  ret.emplace_back(strandIdx, strandElemIdx);

  return ret;
}

StrandSplitInfo::StreamElemIdx
StrandSplitInfo::mapStrandToStream(StrandElemSplitIdx strandElemSplit) const {

  if (this->splitByElem) {
    return this->mapStrandToStreamByElem(strandElemSplit);
  }

  auto strandIdx = strandElemSplit.strandIdx;
  auto strandElemIdx = strandElemSplit.elemIdx;

  auto newRet = this->mapStrandToStreamByDim(strandElemSplit);

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
StrandSplitInfo::getStrandTripCount(TripCount streamTripCount,
                                    StrandIdx strandIdx) const {

  if (this->splitByElem) {
    return this->getStrandTripCountByElem(streamTripCount, strandIdx);
  }

  auto newRet = this->getStrandTripCountByDim(streamTripCount, strandIdx);
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

StrandElemSplitIdx
StrandSplitInfo::mapStreamToStrandByDim(StreamElemIdx streamElemIdx) const {

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

StrandSplitInfo::TripCount
StrandSplitInfo::getStrandTripCountByDim(TripCount streamTripCount,
                                         StrandIdx strandIdx) const {

  if ((streamTripCount % (innerTrip * splitTrip)) != 0) {
    panic("StreamTrip %ld InnerTrip %ld SplitTrip %ld.", streamTripCount,
          innerTrip, splitTrip);
  }
  auto outerTrip = streamTripCount / (innerTrip * splitTrip);

  auto strandSplitTrip = this->getSplitDimTotalTripForStrand(strandIdx);

  return innerTrip * strandSplitTrip * outerTrip;
}

StrandSplitInfo::StreamElemIdx StrandSplitInfo::mapStrandToStreamByDim(
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

std::vector<StrandElemSplitIdx>
StrandSplitInfo::mapStreamToPrevStrandByDim(StreamElemIdx streamElemIdx) const {

  /**
   * We first get the first Strand. And then get TargetStrandId and
   * StreamElemIdx.
   *
   * NOTE: This does not support InitOffset.
   *
   * Define InterleaveCount = StreamElemIdx / (Interleave * TotalStrands).
   * For all strands:
   * 1. If strendId < targetStrandId:
   *    Check that ((InterleaveCount + 1) * Interleave - 1) is Acked.
   * 2. If strandId == targetStrandId:
   *    Check that StreamElemIdx is Acked.
   * 3. If strandId > targetStrandId and InterleaveCount > 0
   *    Check that IntleaveCount * Interleave is Acked.
   *
   */

  std::vector<StrandElemSplitIdx> prevSplits;

  auto targetStrandElemSplit = this->mapStreamToStrand(streamElemIdx);

  auto interleaveCount =
      streamElemIdx / (this->getInterleave() * this->getTotalStrands());
  for (auto strandIdx = 0; strandIdx < this->getTotalStrands(); ++strandIdx) {

    uint64_t checkStrandElemIdx = 0;
    if (strandIdx < targetStrandElemSplit.strandIdx) {
      checkStrandElemIdx = (interleaveCount + 1) * this->getInterleave() - 1;
    } else if (strandIdx > targetStrandElemSplit.strandIdx) {
      checkStrandElemIdx = interleaveCount * this->getInterleave();
    } else {
      checkStrandElemIdx = targetStrandElemSplit.elemIdx;
    }

    prevSplits.emplace_back(strandIdx, checkStrandElemIdx);
  }

  return prevSplits;
}

StrandElemSplitIdx
StrandSplitInfo::mapStreamToStrandByElem(StreamElemIdx streamElemIdx) const {

  auto prevSplitElemIdx = 0;
  auto strandIdx = 0;
  for (; strandIdx < this->streamElemSplits.size(); ++strandIdx) {
    auto splitStreamElemIdx = this->streamElemSplits.at(strandIdx);
    if (streamElemIdx < splitStreamElemIdx) {
      // Found the strand.
      return StrandElemSplitIdx(strandIdx, streamElemIdx - prevSplitElemIdx);
    } else {
      // Go to next strand.
      prevSplitElemIdx = splitStreamElemIdx;
    }
  }
  // This belongs to the last strand.
  return StrandElemSplitIdx(strandIdx, streamElemIdx - prevSplitElemIdx);
}

StrandSplitInfo::StreamElemIdx StrandSplitInfo::mapStrandToStreamByElem(
    StrandElemSplitIdx strandElemIdx) const {
  auto strandIdx = strandElemIdx.strandIdx;
  auto prevSplitElemIdx = 0;
  assert(strandIdx <= this->streamElemSplits.size());
  if (strandIdx > 0) {
    prevSplitElemIdx = this->streamElemSplits.at(strandIdx - 1);
  }
  return prevSplitElemIdx + strandElemIdx.elemIdx;
}

StrandSplitInfo::TripCount
StrandSplitInfo::getStrandTripCountByElem(TripCount streamTripCount,
                                          StrandIdx strandIdx) const {

  auto prevSplitElemIdx = 0;
  auto lastElemIdx = streamTripCount;
  assert(strandIdx <= this->streamElemSplits.size());
  if (strandIdx > 0) {
    prevSplitElemIdx = this->streamElemSplits.at(strandIdx - 1);
  }
  if (strandIdx < this->streamElemSplits.size()) {
    lastElemIdx = this->streamElemSplits.at(strandIdx);
  }
  assert(lastElemIdx >= prevSplitElemIdx);
  return lastElemIdx - prevSplitElemIdx;
}

std::vector<StrandElemSplitIdx> StrandSplitInfo::mapStreamToPrevStrandByElem(
    StreamElemIdx streamElemIdx) const {
  std::vector<StrandElemSplitIdx> prevSplits;

  auto targetStrandElemSplit = this->mapStreamToStrand(streamElemIdx);
  for (auto strandIdx = 0; strandIdx < this->getTotalStrands(); ++strandIdx) {
    if (strandIdx < targetStrandElemSplit.strandIdx) {
      auto checkStreamElemIdx = this->streamElemSplits.at(strandIdx);
      assert(checkStreamElemIdx > 0);
      // Adjust back to strand elem idx.
      checkStreamElemIdx--;

      auto checkStrandSplit = this->mapStreamToStrand(checkStreamElemIdx);
      assert(checkStrandSplit.strandIdx == strandIdx);

      prevSplits.emplace_back(checkStrandSplit);
    } else if (strandIdx > targetStrandElemSplit.strandIdx) {
      // Use -1 to skip this strand.
      prevSplits.emplace_back(StrandElemSplitIdx(strandIdx, -1));
    } else {
      prevSplits.emplace_back(targetStrandElemSplit);
    }
  }

  return prevSplits;
}