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

CacheStreamConfigureDataPtr CacheStreamConfigureData::getUsedByBaseConfig() {
  for (auto &edge : this->baseEdges) {
    if (!edge.isUsedBy) {
      continue;
    }
    auto baseConfig = edge.data.lock();
    if (!baseConfig) {
      DYN_S_PANIC(this->dynamicId, "UsedByBaseConfig %s already released.",
                  edge.dynStreamId);
    }
    return baseConfig;
  }
  DYN_S_PANIC(this->dynamicId, "Failed to get UsedByBaseConfig.");
}

void CacheStreamConfigureData::addUsedBy(CacheStreamConfigureDataPtr &data,
                                         int reuse, bool predBy,
                                         bool predValue) {
  int skip = 0;
  this->depEdges.emplace_back(DepEdge::Type::UsedBy, data, reuse, skip);
  data->baseEdges.emplace_back(BaseEdge::Type::BaseOn, this->shared_from_this(),
                               reuse, skip, true /* isUsedBy */);
  if (predBy) {
    data->baseEdges.back().isPredBy = true;
    data->baseEdges.back().predValue = predValue;
  }
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

void CacheStreamConfigureData::addPUMSendTo(
    const CacheStreamConfigureDataPtr &data, const AffinePattern &broadcastPat,
    const AffinePattern &recvPat, const AffinePattern &recvTile) {
  this->depEdges.emplace_back(
      CacheStreamConfigureData::DepEdge::Type::PUMSendTo, data, 1 /* reuse */,
      0 /* skip */);
  this->depEdges.back().broadcastPat = broadcastPat;
  this->depEdges.back().recvPat = recvPat;
  this->depEdges.back().recvTile = recvTile;
}

void CacheStreamConfigureData::addBaseOn(CacheStreamConfigureDataPtr &data,
                                         int reuse, int skip) {
  if (reuse <= 0 || skip < 0) {
    panic("Illegal BaseOn Reuse %d Skip %d This %s -> Base %s.", reuse, skip,
          this->dynamicId, data->dynamicId);
  }
  this->baseEdges.emplace_back(BaseEdge::Type::BaseOn, data, reuse, skip);
}

void CacheStreamConfigureData::addBaseAffineIV(
    CacheStreamConfigureDataPtr &data, int reuse, int skip) {
  if (reuse <= 0 || skip < 0) {
    panic("Illegal BaseAffineIV Reuse %d Skip %d This %s -> Base %s.", reuse,
          skip, this->dynamicId, data->dynamicId);
  }
  this->baseEdges.emplace_back(data, reuse, skip);
}

void CacheStreamConfigureData::addPredBy(CacheStreamConfigureDataPtr &data,
                                         int reuse, int skip, bool predValue) {
  if (reuse <= 0 || skip < 0) {
    panic("Illegal PredBy Reuse %d Skip %d This %s -> Base %s.", reuse, skip,
          this->dynamicId, data->dynamicId);
  }
  this->baseEdges.emplace_back(data, reuse, skip, predValue);
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

bool CacheStreamConfigureData::sendToInnerLoopStream() const {
  auto check = [this](const CacheStreamConfigureData &config) -> bool {
    for (const auto &e : config.depEdges) {
      if (e.type == CacheStreamConfigureData::DepEdge::Type::SendTo) {
        const auto &recvConfig = e.data;
        if (recvConfig->stream->getLoopLevel() > this->stream->getLoopLevel()) {
          return true;
        }
      }
    }
    return false;
  };
  if (check(*this)) {
    return true;
  }
  for (const auto &e : this->depEdges) {
    if (e.type == CacheStreamConfigureData::DepEdge::Type::UsedBy) {
      const auto &indConfig = e.data;
      if (check(*indConfig)) {
        return true;
      }
    }
  }
  return false;
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
  auto interleave = strandSplit.getInterleave();
  auto totalStrands = strandSplit.getTotalStrands();
  auto strandTripCount = strandSplit.getStrandTripCount(tripCount, strandIdx);

  if (strandTripCount >= interleave) {
    assert(strandTripCount % interleave == 0);
  }

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
  if (strandTripCount < interleave) {
    addStrandParam(strandTripCount);
  } else {
    addStrandParam(interleave);
  }
  addStrandParam(strandStride);
  addStrandParam(strandTripCount);
  addStrandParam(strandStart);
  DYN_S_DPRINTF_(MLCRubyStrandSplit, this->dynamicId, "Split 1D Continuous.\n");
  DYN_S_DPRINTF_(MLCRubyStrandSplit, this->dynamicId,
                 "start %#x stride %d tripCount %llu.\n", start, stride,
                 tripCount);
  DYN_S_DPRINTF_(MLCRubyStrandSplit, this->dynamicId,
                 "interleave %d totalStrands %llu.\n", interleave,
                 totalStrands);
  DYN_S_DPRINTF_(MLCRubyStrandSplit, this->dynamicId,
                 "strandStart %#x strandStride %d strandTripCount %lu.\n",
                 strandStart, strandStride, strandTripCount);

  return strandParams;
}

DynStreamFormalParamV CacheStreamConfigureData::splitAffinePatternAtDim(
    int splitDim, int64_t interleave, int strandIdx, int totalStrands) {
  const auto &params = this->addrGenFormalParams;
  auto callback = this->addrGenCallback;

  auto linearAddrGen =
      std::dynamic_pointer_cast<LinearAddrGenCallback>(callback);
  assert(linearAddrGen && "Callback is not linear.");

  /**
   * * Split an AffineStream at SplitDim. This is similar to OpenMP static
   * * scheduling.
   * *   start : S1 : T1 : ... : Ss : Ts : ... : Sn : Tn
   *
   * * We assume interleave % (T1 * ... * Ts-1) == 0, and define
   * * Tt = interleave / (T1 * ... * Ts-1)
   * * Tn = Tt * totalStrands
   *
   * * ->
   * *   start + strandIdx * Ss * Tt
   * * : S1      : T1 : ...
   * * : Ss      : Tt
   * * : Ss * Tn : Ts / Tn + (strandIdx < (Ts % Tn) ? 1 : 0) : ...
   * * : Sn      : Tn
   *
   * * Notice that we have to take care when Ts % Tn != 0 by adding one to
   * * strands with strandIdx < (Ts % Tn).
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

  auto innerTrip = 1;
  for (int i = 0; i < splitDim; ++i) {
    innerTrip *= trips.at(i);
  }
  assert(interleave % innerTrip == 0);
  auto intrlvTrip = interleave / innerTrip;
  auto totalIntrlvTrip = intrlvTrip * totalStrands;

  auto start = params.back().invariant.uint64();
  auto strandStart = start + strandIdx * splitDimStride * intrlvTrip;

  // Copy the original params.
  DynStreamFormalParamV strandParams = this->addrGenFormalParams;

#define setTrip(dim, t)                                                        \
  {                                                                            \
    strandParams.at((dim)*2 + 1).isInvariant = true;                           \
    strandParams.at((dim)*2 + 1).invariant.uint64() = t;                       \
  }
#define setStride(dim, t)                                                      \
  {                                                                            \
    strandParams.at((dim)*2).isInvariant = true;                               \
    strandParams.at((dim)*2).invariant.uint64() = t;                           \
  }
#define setStart(t)                                                            \
  {                                                                            \
    strandParams.back().isInvariant = true;                                    \
    strandParams.back().invariant.uint64() = t;                                \
  }

  // Insert another dimension after SplitDim.
  strandParams.insert(strandParams.begin() + 2 * splitDim + 1,
                      DynStreamFormalParam());
  strandParams.insert(strandParams.begin() + 2 * splitDim + 1,
                      DynStreamFormalParam());

  // Adjust the strand start.
  setStart(strandStart);

  int64_t splitOutTrip = 1;
  int64_t splitTrip = intrlvTrip;

  DYN_S_DPRINTF_(
      MLCRubyStrandSplit, this->dynamicId,
      "Intrlv %d IntrlvTrip %d SplitDimTrip %d TotalStrands %d Pat %s.\n",
      interleave, intrlvTrip, splitDimTrip, totalStrands,
      printAffinePatternParams(this->addrGenFormalParams));

  if (totalIntrlvTrip <= splitDimTrip) {
    // Compute the SplitOutTrip.
    auto remainderTrip = splitDimTrip % totalIntrlvTrip;
    if (remainderTrip % intrlvTrip != 0) {
      if (splitDim + 1 != trips.size()) {
        DYN_S_PANIC(this->dynamicId,
                    "Cannot handle remainderTrip %ld % intrlvTrip %ld != 0.",
                    remainderTrip, intrlvTrip);
      }
    }
    auto remainderStrandIdx = (remainderTrip + intrlvTrip - 1) / intrlvTrip;
    auto splitOutTripRemainder = (strandIdx < remainderStrandIdx) ? 1 : 0;
    splitOutTrip = splitDimTrip / totalIntrlvTrip + splitOutTripRemainder;

  } else {
    /**
     * Strands beyond FinalStrandIdx would have no trip count.
     */
    auto finalStrandIdx = splitDimTrip / intrlvTrip;
    if (strandIdx == finalStrandIdx) {
      splitTrip = splitDimTrip - finalStrandIdx * intrlvTrip;
    } else if (strandIdx > finalStrandIdx) {
      splitTrip = 0;
    }

    // In this case, SplitOutDimTrip is always 1.
    splitOutTrip = 1;
  }

  // Adjust the SplitOutDim.
  setTrip(splitDim, splitTrip * innerTrip);
  setStride(splitDim + 1, splitDimStride * totalIntrlvTrip);
  assert(splitOutTrip > 0);
  setTrip(splitDim + 1, splitOutTrip * splitTrip * innerTrip);

  // We need to fix all upper dimension's trip count.
  for (int dim = splitDim + 2; dim < trips.size() + 1; ++dim) {
    auto fixedOuterTrip =
        strandParams.at(dim * 2 - 1).invariant.uint64() * trips.at(dim - 1);
    setTrip(dim, fixedOuterTrip);
  }

#undef setTrip
#undef setStride
#undef setStart

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
                       this->strandSplit.getTotalStrands());
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
