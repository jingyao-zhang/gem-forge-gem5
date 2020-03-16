#include "CacheStreamConfigureData.hh"

CacheStreamConfigureData::CacheStreamConfigureData(
    Stream *_stream, const DynamicStreamId &_dynamicId, int _elementSize,
    const std::vector<DynamicStreamFormalParam> &_addrGenFormalParams,
    std::shared_ptr<AddrGenCallback> _addrGenCallback)
    : stream(_stream), dynamicId(_dynamicId), elementSize(_elementSize),
      initVAddr(0), initPAddr(0), addrGenFormalParams(_addrGenFormalParams),
      addrGenCallback(_addrGenCallback), isPointerChase(false),
      isOneIterationBehind(false), initAllocatedIdx(0) {
  assert(this->addrGenCallback && "Invalid addrGenCallback.");
}

CacheStreamConfigureData::CacheStreamConfigureData(
    const CacheStreamConfigureData &other)
    : stream(other.stream), dynamicId(other.dynamicId),
      elementSize(other.elementSize), initVAddr(other.initVAddr),
      initPAddr(other.initPAddr), initPAddrValid(other.initPAddrValid),
      isPseudoOffload(other.isPseudoOffload),
      addrGenFormalParams(other.addrGenFormalParams),
      addrGenCallback(other.addrGenCallback),
      predFormalParams(other.predFormalParams),
      predCallback(other.predCallback), totalTripCount(other.totalTripCount),
      constUpdateValue(other.constUpdateValue),
      isPredicated(other.isPredicated),
      isPredicatedTrue(other.isPredicatedTrue),
      predicateStreamId(other.predicateStreamId),
      storeFormalParams(other.storeFormalParams),
      storeCallback(other.storeCallback),
      reductionInitValue(other.reductionInitValue),
      isPointerChase(other.isPointerChase),
      isOneIterationBehind(other.isOneIterationBehind),
      indirectStreams(other.indirectStreams),
      initAllocatedIdx(other.initAllocatedIdx) {}