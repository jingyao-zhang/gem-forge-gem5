#include "StrandSplitInfo.hh"

#include "base/logging.hh"
#include "base/trace.hh"

#include "debug/MLCRubyStrandSplit.hh"

#include <cassert>

namespace gem5 {

// StrandSplitInfo::StrandSplitInfo(int64_t _innerTrip, int64_t _splitTrip,
//                                  int64_t _splitTripPerStrand,
//                                  int64_t _totalStrands)
//     : totalStrands(_totalStrands), splitByDim(true), innerTrip(_innerTrip),
//       splitTrip(_splitTrip), splitTripPerStrand(_splitTripPerStrand) {}

StrandSplitInfo::StrandSplitInfo(const IntVecT &trips,
                                 const std::vector<SplitDim> &splitDims)
    : splitByDim(true) {

  for (auto trip : trips) {
    // Add non-split dimension first.
    this->dimensions.emplace_back(trip);
  }

  for (const auto &splitDim : splitDims) {

    assert(splitDim.dim >= 0 && splitDim.dim < this->dimensions.size());
    assert(splitDim.cnt >= 1);
    assert(splitDim.intrlv >= 1);

    auto &dim = this->dimensions.at(splitDim.dim);
    dim.splitCnt = splitDim.cnt;
    dim.splitIntrlv = splitDim.intrlv;
  }

  int64_t accTrip = 1;
  int64_t accSplitCnt = 1;
  for (auto &dim : this->dimensions) {
    dim.accTrip = accTrip;
    dim.accSplitCnt = accSplitCnt;

    accTrip *= dim.trip;
    accSplitCnt *= dim.splitCnt;
  }
  this->totalStrands = accSplitCnt;
}

StrandSplitInfo::StrandSplitInfo(
    const std::vector<StreamElemIdx> &_streamElemSplits, int64_t _totalStrands)
    : totalStrands(_totalStrands), splitByElem(true),
      streamElemSplits(_streamElemSplits) {}

int64_t StrandSplitInfo::Dimension::getStrandId(int64_t streamOffset) const {
  return (streamOffset / this->splitIntrlv) % this->splitCnt;
}

int64_t
StrandSplitInfo::Dimension::getStrandOffset(int64_t streamOffset) const {
  return (streamOffset / this->splitIntrlv / this->splitCnt) *
             this->splitIntrlv +
         streamOffset % this->splitIntrlv;
}

int64_t
StrandSplitInfo::Dimension::getStreamOffset(int64_t strandId,
                                            int64_t strandOffset) const {
  return (strandOffset / this->splitIntrlv) * this->splitIntrlv *
             this->splitCnt +
         strandId * this->splitIntrlv + strandOffset % this->splitIntrlv;
}

int64_t StrandSplitInfo::Dimension::getStrandTrip(int64_t strandId) const {
  auto lastRound =
      (this->trip / this->splitIntrlv / this->splitCnt) * this->splitIntrlv;
  if (strandId < this->lastStrandId) {
    return lastRound + this->splitIntrlv;
  } else if (strandId == this->lastStrandId) {
    return lastRound + this->trip % this->splitIntrlv;
  } else {
    return lastRound;
  }
}

StrandElemSplitIdx
StrandSplitInfo::mapStreamToStrand(StreamElemIdx streamElemIdx) const {

  if (this->splitByElem) {
    return this->mapStreamToStrandByElem(streamElemIdx);
  }

  if (this->splitByDim) {
    return this->mapStreamToStrandByDim(streamElemIdx);
  }

  assert(this->totalStrands == 1);
  return StrandElemSplitIdx(0, streamElemIdx);
}

std::vector<StrandElemSplitIdx>
StrandSplitInfo::mapStreamToPrevStrand(StreamElemIdx streamElemIdx) const {

  if (this->splitByElem) {
    return this->mapStreamToPrevStrandByElem(streamElemIdx);
  }
  if (this->splitByDim) {
    return this->mapStreamToPrevStrandByDim(streamElemIdx);
  }
  panic("Invalid StrandSplitInfo.");
  assert(this->totalStrands == 1);
  std::vector<StrandElemSplitIdx> ret{{0, streamElemIdx}};
  return ret;
}

StrandSplitInfo::StreamElemIdx
StrandSplitInfo::mapStrandToStream(StrandElemSplitIdx strandElemSplit) const {

  if (this->splitByElem) {
    return this->mapStrandToStreamByElem(strandElemSplit);
  }

  if (this->splitByDim) {
    return this->mapStrandToStreamByDim(strandElemSplit);
  }
  assert(this->totalStrands == 1);
  assert(strandElemSplit.strandIdx == 0);
  return strandElemSplit.elemIdx;
}

StrandSplitInfo::TripCount
StrandSplitInfo::getStrandTripCount(TripCount streamTripCount,
                                    StrandIdx strandIdx) const {

  if (this->splitByElem) {
    return this->getStrandTripCountByElem(streamTripCount, strandIdx);
  }

  if (this->splitByDim) {
    return this->getStrandTripCountByDim(strandIdx);
  }

  assert(this->totalStrands == 1);
  assert(strandIdx == 0);
  return streamTripCount;
}

StrandElemSplitIdx
StrandSplitInfo::mapStreamToStrandByDim(StreamElemIdx streamElemIdx) const {

  TripCount accStrandTrip = 1;
  StrandIdx accStrandId = 0;
  StreamElemIdx accStrandElemIdx = 0;
  for (const auto &dim : this->dimensions) {
    auto streamOffset = (streamElemIdx / dim.accTrip) % dim.trip;

    auto strandId = dim.getStrandId(streamOffset);
    auto strandOffset = dim.getStrandOffset(streamOffset);
    auto strandTrip = dim.getStrandTrip(strandId);

    accStrandId += strandId * dim.accSplitCnt;
    accStrandElemIdx += strandOffset * accStrandTrip;

    accStrandTrip *= strandTrip;
  }
  return StrandElemSplitIdx(accStrandId, accStrandElemIdx);
}

StrandSplitInfo::TripCount StrandSplitInfo::getStrandTripCountByDim(
    StrandIdx strandIdx, int untilDim) const {

  auto endDim = this->dimensions.size();
  if (untilDim != -1) {
    endDim = untilDim;
  }
  assert(endDim <= this->dimensions.size());

  StrandIdx accSplitCnt = 1;
  TripCount accStrandTrip = 1;

  for (int i = 0; i < endDim; ++i) {
    const auto &dim = this->dimensions.at(i);

    auto strandId = strandIdx % accSplitCnt;
    auto strandTrip = dim.getStrandTrip(strandId);

    accSplitCnt *= dim.splitCnt;
    accStrandTrip *= strandTrip;
  }
  return accStrandTrip;
}

StrandSplitInfo::StreamElemIdx StrandSplitInfo::mapStrandToStreamByDim(
    StrandElemSplitIdx strandElemSplit) const {
  auto strandIdx = strandElemSplit.strandIdx;
  auto strandElemIdx = strandElemSplit.elemIdx;

  TripCount accStrandTrip = 1;
  StreamElemIdx accStreamElemIdx = 0;
  for (const auto &dim : this->dimensions) {

    auto strandId = (strandIdx / dim.accSplitCnt) % dim.splitCnt;
    auto strandTrip = dim.getStrandTrip(strandId);

    auto strandOffset = (strandElemIdx / accStrandTrip) % strandTrip;
    auto streamOffset = dim.getStreamOffset(strandId, strandOffset);

    accStreamElemIdx += streamOffset * dim.accTrip;

    accStrandTrip *= strandTrip;
  }
  return accStreamElemIdx;
}

void StrandSplitInfo::mapStreamToPrevStrandByDimImpl(
    MapToPrevStrandByDimContext &context) const {

  if (context.dim == -1) {
    // Reached the bottom.
    int64_t accStrandId = 0;
    int64_t accStrandElemIdx = 0;
    int64_t accStrandTrip = 1;
    for (int i = 0; i < this->dimensions.size(); ++i) {
      const auto &dim = this->dimensions.at(i);
      auto strandId = context.strandIds.at(i);
      accStrandId += strandId * dim.accSplitCnt;
      accStrandElemIdx += context.strandOffsets.at(i) * accStrandTrip;
      accStrandTrip *= dim.getStrandTrip(strandId);
    }
    if (accStrandElemIdx >= 0) {
      // Skip those strands with negative element idx.
      context.ret.emplace_back(accStrandId, accStrandElemIdx);
    }
    return;
  }

  auto curDim = context.dim;
  auto nextDim = curDim - 1;
  const auto &dim = this->dimensions.at(curDim);

  auto roundUp = [](int64_t x, int64_t y) -> int64_t {
    return x + (y - x % y - 1);
  };
  auto roundDown = [](int64_t x, int64_t y) -> int64_t {
    return x - x % y - 1;
  };

  auto strandOffset = context.strandOffsets.at(curDim);
  auto strandOffsetUp = roundUp(strandOffset, dim.splitIntrlv);
  auto strandOffsetDown = roundDown(strandOffset, dim.splitIntrlv);
  if (context.lessThanTargetStrandId) {

    /**
     * If I am already smaller strand id than target. Simply adjust the
     * remaining dimensions to the StrandTrip.
     */
    for (int i = 0; i < dim.splitCnt; ++i) {
      context.strandIds.at(curDim) = i;
      context.strandOffsets.at(curDim) = dim.getStrandTrip(i) - 1;
      context.dim = nextDim;
      this->mapStreamToPrevStrandByDimImpl(context);
      context.dim = curDim;
      context.strandOffsets.at(curDim) = strandOffset;
    }
    return;
  }

  /**
   * Otherwise, we take care of whether it's the same target or not.
   */
  for (int i = 0; i < dim.splitCnt; ++i) {
    context.strandIds.at(curDim) = i;
    if (i < context.targetStrandIds.at(curDim)) {
      // Less than, we can round up.
      context.strandOffsets.at(curDim) = strandOffsetUp;
      context.lessThanTargetStrandId = true;
      context.dim = nextDim;
      this->mapStreamToPrevStrandByDimImpl(context);
      context.dim = curDim;
      context.lessThanTargetStrandId = false;
      context.strandOffsets.at(curDim) = strandOffset;
    } else if (i > context.targetStrandIds.at(curDim)) {
      // Greater than, round down.
      context.strandOffsets.at(curDim) = strandOffsetDown;
      context.lessThanTargetStrandId = true;
      context.dim = nextDim;
      this->mapStreamToPrevStrandByDimImpl(context);
      context.dim = curDim;
      context.lessThanTargetStrandId = false;
      context.strandOffsets.at(curDim) = strandOffset;
    } else {
      // Same dim.
      context.dim = nextDim;
      this->mapStreamToPrevStrandByDimImpl(context);
      context.dim = curDim;
    }
  }
}

std::vector<StrandElemSplitIdx>
StrandSplitInfo::mapStreamToPrevStrandByDim(StreamElemIdx streamElemIdx) const {

  IntVecT targetStrandIds;
  IntVecT targetStrandOffsets;

  for (const auto &dim : this->dimensions) {
    auto streamOffset = (streamElemIdx / dim.accTrip) % dim.trip;

    auto strandId = dim.getStrandId(streamOffset);
    auto strandOffset = dim.getStrandOffset(streamOffset);
    targetStrandIds.push_back(strandId);
    targetStrandOffsets.push_back(strandOffset);
  }

  MapToPrevStrandByDimContext context;
  context.dim = this->dimensions.size() - 1;
  context.targetStrandIds = targetStrandIds;
  context.strandIds = targetStrandIds;
  context.strandOffsets = targetStrandOffsets;
  context.lessThanTargetStrandId = false;
  this->mapStreamToPrevStrandByDimImpl(context);
  return context.ret;
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
} // namespace gem5
