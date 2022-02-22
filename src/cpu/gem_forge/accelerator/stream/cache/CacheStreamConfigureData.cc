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

void CacheStreamConfigureData::addSendTo(CacheStreamConfigureDataPtr &data) {
  for (const auto &edge : this->depEdges) {
    if (edge.type == DepEdge::Type::SendTo && edge.data == data) {
      // This is already here.
      return;
    }
  }
  this->depEdges.emplace_back(DepEdge::Type::SendTo, data);
}

DynStreamFormalParamV CacheStreamConfigureData::splitLinearParam(
    const StrandSplitInfo &strandSplit, int strandIdx,
    const DynStreamFormalParamV &params, AddrGenCallbackPtr callback) {
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

CacheStreamConfigureVec
CacheStreamConfigureData::splitIntoStrands(const StrandSplitInfo &strandSplit) {
  assert(this->totalStrands == 1 && "Already splited.");
  assert(this->strandIdx == 0 && "Already splited.");
  assert(this->strandSplit.totalStrands == 1 && "Already splited.");
  assert(this->streamConfig == nullptr && "This is a strand.");
  assert(this->isPseudoOffload == false && "Split PseudoOffload.");
  assert(this->rangeSync == false && "Split RangeSync.");
  assert(this->rangeCommit == false && "Split RangeCommit.");
  assert(this->hasBeenCuttedByMLC == false && "Split MLC cut.");
  assert(this->isPointerChase == false && "Split pointer chase.");
  assert(this->isOneIterationBehind == false && "Split pointer chase.");
  assert(strandSplit.totalStrands > 1 && "Only 1 strand.");

  this->strandSplit = strandSplit;
  this->totalStrands = strandSplit.totalStrands;

  CacheStreamConfigureVec strands;

  for (auto strandIdx = 0; strandIdx < strandSplit.totalStrands; ++strandIdx) {

    /**********************************************************************
     * Split the address generation.
     **********************************************************************/
    auto strandAddrGenFormalParams = this->splitLinearParam(
        strandSplit, strandIdx, this->addrGenFormalParams,
        this->addrGenCallback);

    auto strand = std::make_shared<CacheStreamConfigureData>(
        this->stream, this->dynamicId, this->elementSize,
        strandAddrGenFormalParams, this->addrGenCallback);
    strands.emplace_back(strand);

    /***************************************************************************
     * Properly set the splited fields.
     ***************************************************************************/

#define copyToStrand(X) strand->X = this->X
    copyToStrand(floatPlan);
    copyToStrand(mlcBufferNumSlices);
    copyToStrand(isPseudoOffload);
    copyToStrand(rangeSync);
    copyToStrand(rangeCommit);
    copyToStrand(hasBeenCuttedByMLC);
    copyToStrand(isPredicated);
    copyToStrand(isPredicatedTrue);
    copyToStrand(predicateStreamId);
    copyToStrand(storeFormalParams);
    copyToStrand(storeCallback);
    copyToStrand(loadFormalParams);
    copyToStrand(loopBoundFormalParams);
    copyToStrand(loopBoundCallback);
    copyToStrand(loopBoundRet);
    copyToStrand(reductionInitValue);
    copyToStrand(finalValueNeededByCore);
    copyToStrand(isPointerChase);
    copyToStrand(isOneIterationBehind);

    // Strand specific field.
    strand->strandIdx = strandIdx;
    strand->totalStrands = strandSplit.totalStrands;
    strand->strandSplit = strandSplit;
    strand->streamConfig = shared_from_this();
    strand->totalTripCount =
        strandSplit.getStrandTripCount(this->getTotalTripCount(), strandIdx);
    strand->initVAddr = makeLineAddress(
        this->addrGenCallback
            ->genAddr(0, strandAddrGenFormalParams, getStreamValueFail)
            .front());
    if (this->stream->getCPUDelegator()->translateVAddrOracle(
            strand->initVAddr, strand->initPAddr)) {
      strand->initPAddrValid = true;
    } else {
      DynStrandId strandId(this->dynamicId, strandIdx,
                           strandSplit.totalStrands);
      panic("%s: Strand InitVAddr %#x faulted.", strandId, strand->initVAddr);
    }

    for (auto &dep : depEdges) {
      assert(dep.type == DepEdge::Type::SendTo && "Split Indirect.");
      strand->addSendTo(dep.data);
    }
    for (auto &base : baseEdges) {
      auto baseConfig = base.data.lock();
      assert(baseConfig && "BaseConfig already released?");
      strand->addBaseOn(baseConfig);
    }
  }

  return strands;
}

DynStrandId CacheStreamConfigureData::getStrandIdFromStreamElemIdx(
    uint64_t streamElemIdx) const {
  assert(this->streamConfig == nullptr &&
         "This is not StreamConfig but a StrandConfig.");
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
  assert(this->streamConfig == nullptr &&
         "This is not StreamConfig but a StrandConfig.");
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
  if (this->streamConfig) {
    // This is a strand.
    StrandElemSplitIdx elemSplit(this->strandIdx, strandElemIdx);
    return this->strandSplit.mapStrandToStream(elemSplit);
  } else {
    // This is not a strand.
    return strandElemIdx;
  }
}