#include "CacheStreamConfigureData.hh"
#include "../stream.hh"

#include "debug/MLCRubyStrandSplit.hh"
#include "debug/StreamLoopBound.hh"

#define DEBUG_TYPE StreamLoopBound
#include "../stream_log.hh"

CacheStreamConfigureData::CacheStreamConfigureData(
    Stream *_stream, const DynStreamId &_dynamicId, int _elementSize,
    const DynStreamFormalParamV &_addrGenFormalParams,
    AddrGenCallbackPtr _addrGenCallback)
    : stream(_stream), dynamicId(_dynamicId), elementSize(_elementSize),
      initVAddr(0), initPAddr(0), addrGenFormalParams(_addrGenFormalParams),
      addrGenCallback(_addrGenCallback), isPointerChase(false),
      isOneIterationBehind(false), initCreditedIdx(0) {
  assert(this->addrGenCallback && "Invalid addrGenCallback.");
}

CacheStreamConfigureData::~CacheStreamConfigureData() {}

void CacheStreamConfigureData::addUsedBy(CacheStreamConfigureDataPtr &data) {
  int reuse = 1;
  int skip = 0;
  this->depEdges.emplace_back(DepEdge::Type::UsedBy, data, reuse, skip);
  data->baseEdges.emplace_back(BaseEdge::Type::BaseOn, this->shared_from_this(),
                               reuse, skip);
}

void CacheStreamConfigureData::addSendTo(CacheStreamConfigureDataPtr &data,
                                         int reuse, int skip) {
  for (const auto &edge : this->depEdges) {
    if (edge.type == DepEdge::Type::SendTo && edge.data == data) {
      // This is already here.
      assert(edge.reuse == reuse && "Mismatch Reuse in SendTo.");
      assert(edge.skip == skip && "Mismatch Skip in SendTo.");
      return;
    }
  }
  this->depEdges.emplace_back(DepEdge::Type::SendTo, data, reuse, skip);
}

void CacheStreamConfigureData::addBaseOn(CacheStreamConfigureDataPtr &data,
                                         int reuse, int skip) {
  if (reuse <= 0 || skip < 0) {
    panic("Illegal BaseOn Reuse %d Skip %d This %s -> Base %s.", reuse, skip,
          this->dynamicId, data->dynamicId);
  }
  this->baseEdges.emplace_back(BaseEdge::Type::BaseOn, data, reuse, skip);
}

uint64_t CacheStreamConfigureData::convertBaseToDepElemIdx(uint64_t baseElemIdx,
                                                           int reuse,
                                                           int skip) {
  auto depElemIdx = baseElemIdx;
  if (reuse != 1) {
    assert(skip == 0);
    depElemIdx = baseElemIdx * reuse;
  }
  if (skip != 0) {
    assert(reuse == 1);
    depElemIdx = baseElemIdx / skip;
  }
  return depElemIdx;
}

uint64_t CacheStreamConfigureData::convertDepToBaseElemIdx(uint64_t depElemIdx,
                                                           int reuse,
                                                           int skip) {
  auto baseElemIdx = depElemIdx;
  if (reuse != 1) {
    assert(skip == 0);
    baseElemIdx = depElemIdx / reuse;
  }
  if (skip != 0) {
    assert(reuse == 1);
    baseElemIdx = depElemIdx * skip;
  }
  return baseElemIdx;
}

DynStreamFormalParamV
CacheStreamConfigureData::splitLinearParam1D(const StrandSplitInfo &strandSplit,
                                             int strandIdx) {

  const auto &params = this->addrGenFormalParams;
  auto callback = this->addrGenCallback;

  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(callback);
  assert(linearAddrGen && "Callback is not linear.");
  assert(params.size() == 3 && "Only support 1D linear pattern so far.");
  assert(strandSplit.initOffset == 0 && "Does not support InitOffset yet.");

  /**
   * * Split an 1D stream pattern of:
   * *   start : stride : tripCount
   * * ->
   * *   start + strandIdx * interleave * stride
   * * : stride
   * * : interleave
   * * : totalStrands * interleave * stride
   * * : strandTripCount
   */
  auto start = params.at(2).invariant.uint64();
  auto stride = params.at(0).invariant.uint64();
  auto tripCount = params.at(1).invariant.uint64();
  auto interleave = strandSplit.interleave;
  auto totalStrands = strandSplit.totalStrands;
  auto strandTripCount = strandSplit.getStrandTripCount(tripCount, strandIdx);

  auto strandStart = start + strandIdx * interleave * stride;
  auto strandStride = totalStrands * interleave * stride;

  DynStreamFormalParamV strandParams;

#define addStrandParam(x)                                                      \
  {                                                                            \
    strandParams.emplace_back();                                               \
    strandParams.back().isInvariant = true;                                    \
    strandParams.back().invariant.uint64() = x;                                \
  }
  addStrandParam(stride);
  addStrandParam(interleave);
  addStrandParam(strandStride);
  addStrandParam(strandTripCount);
  addStrandParam(strandStart);
  hack("start %#x stride %d tripCount %llu.\n", start, stride, tripCount);
  hack("interleave %d initOffset %d totalStrands %llu.\n", interleave,
       strandSplit.initOffset, totalStrands);
  hack("strandStart %#x strandStride %d strandTripCount %lu.\n", strandStart,
       strandStride, strandTripCount);

  return strandParams;
}

DynStreamFormalParamV
CacheStreamConfigureData::splitAffinePatternAtDim(int splitDim, int strandIdx,
                                                  int totalStrands) {
  const auto &params = this->addrGenFormalParams;
  auto callback = this->addrGenCallback;

  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(callback);
  assert(linearAddrGen && "Callback is not linear.");

  /**
   * * Split an AffineStream at SplitDim. This is similar to OpenMP static
   * * scheduling.
   * *   start : S1 : T1 : ... : Ss : Ts : ... : Sn : Tn
   * * ->
   * *   start + strandIdx * Ss * Ts / totalStrands
   * * : S1 : T1 : ...
   * * : Ss : Ts / totalStrands : ...
   * * : Sn : Tn
   */

  std::vector<uint64_t> trips;
  std::vector<int64_t> strides;
  uint64_t prevTrip = 1;
  assert((this->addrGenFormalParams.size() % 2) == 1);
  for (int i = 0; i + 1 < this->addrGenFormalParams.size(); i += 2) {
    const auto &s = this->addrGenFormalParams.at(i);
    assert(s.isInvariant);
    strides.push_back(s.invariant.int64());

    const auto &t = this->addrGenFormalParams.at(i + 1);
    assert(t.isInvariant);
    auto trip = t.invariant.uint64();
    trips.push_back(trip / prevTrip);
    prevTrip = trip;
  }
  assert(!trips.empty());
  assert(splitDim < trips.size());

  auto splitDimTrip = trips.at(splitDim);
  auto splitDimStride = strides.at(splitDim);
  auto splitDimTripPerStrand = (splitDimTrip + totalStrands - 1) / totalStrands;

  auto start = params.back().invariant.uint64();

  auto strandStart = start + strandIdx * splitDimStride * splitDimTripPerStrand;

  // Copy the original params.
  DynStreamFormalParamV strandParams = this->addrGenFormalParams;

  // Adjust the trip count at the SplitDim. Be careful with the last strand.
  auto splitDimStrandTrip = splitDimTripPerStrand;
  if (strandIdx == totalStrands - 1) {
    // Adjust the last strand.
    splitDimStrandTrip = splitDimTrip - (strandIdx * splitDimTripPerStrand);
  }
  strandParams.at(splitDim * 2 + 1).invariant.uint64() = splitDimStrandTrip;
  // We need to fix all upper dimension's trip count.
  for (int dim = splitDim + 1; dim < trips.size(); ++dim) {
    auto fixedOuterTrip =
        strandParams.at(dim * 2 - 1).invariant.uint64() * trips.at(dim);
    strandParams.at(dim * 2 + 1).invariant.uint64() = fixedOuterTrip;
  }

  // Adjust the strand start.
  strandParams.back().invariant.uint64() = strandStart;

  return strandParams;
}

DynStrandId CacheStreamConfigureData::getStrandIdFromStreamElemIdx(
    uint64_t streamElemIdx) const {
  if (this->streamConfig) {
    // This is a StrandConfig.
    return this->streamConfig->getStrandIdFromStreamElemIdx(streamElemIdx);
  }
  if (this->totalStrands == 1) {
    // There is no strand.
    return DynStrandId(this->dynamicId);
  } else {
    auto strandElemSplit = this->strandSplit.mapStreamToStrand(streamElemIdx);
    return DynStrandId(this->dynamicId, strandElemSplit.strandIdx,
                       this->strandSplit.totalStrands);
  }
}

uint64_t CacheStreamConfigureData::getStrandElemIdxFromStreamElemIdx(
    uint64_t streamElemIdx) const {
  if (this->streamConfig) {
    // This is a StrandConfig.
    return this->streamConfig->getStrandElemIdxFromStreamElemIdx(streamElemIdx);
  }
  if (this->totalStrands == 1) {
    // There is no strand.
    return streamElemIdx;
  } else {
    auto strandElemSplit = this->strandSplit.mapStreamToStrand(streamElemIdx);
    return strandElemSplit.elemIdx;
  }
}

uint64_t CacheStreamConfigureData::getStreamElemIdxFromStrandElemIdx(
    uint64_t strandElemIdx) const {
  if (!this->isSplitIntoStrands()) {
    // If not splitted, StrandElemIdx == StreamElemIdx.
    return strandElemIdx;
  }
  assert(this->streamConfig && "We need StrandConfig");
  // This is a strand.
  StrandElemSplitIdx elemSplit(this->strandIdx, strandElemIdx);
  return this->strandSplit.mapStrandToStream(elemSplit);
}

uint64_t CacheStreamConfigureData::getStreamElemIdxFromStrandElemIdx(
    const DynStrandId &strandId, uint64_t strandElemIdx) const {
  StrandElemSplitIdx elemSplit(strandId.strandIdx, strandElemIdx);
  return this->strandSplit.mapStrandToStream(elemSplit);
}
