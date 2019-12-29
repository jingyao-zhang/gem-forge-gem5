#include "CacheStreamConfigureData.hh"

CacheStreamConfigureData::CacheStreamConfigureData(
    Stream *_stream, const DynamicStreamId &_dynamicId, int _elementSize,
    const std::vector<DynamicStreamFormalParam> &_formalParams,
    std::shared_ptr<AddrGenCallback> _addrGenCallback)
    : stream(_stream), dynamicId(_dynamicId), elementSize(_elementSize),
      initVAddr(0), initPAddr(0), formalParams(_formalParams),
      addrGenCallback(_addrGenCallback), isPointerChase(false),
      isOneIterationBehind(false), indirectStreamConfigure(nullptr),
      initAllocatedIdx(0) {
  assert(this->addrGenCallback && "Invalid addrGenCallback.");
}

CacheStreamConfigureData::CacheStreamConfigureData(
    const CacheStreamConfigureData &other)
    : stream(other.stream), dynamicId(other.dynamicId),
      elementSize(other.elementSize), initVAddr(other.initVAddr),
      initPAddr(other.initPAddr), formalParams(other.formalParams),
      addrGenCallback(other.addrGenCallback),
      isPointerChase(other.isPointerChase),
      isOneIterationBehind(other.isOneIterationBehind),
      indirectStreamConfigure(other.indirectStreamConfigure),
      initAllocatedIdx(other.initAllocatedIdx) {}