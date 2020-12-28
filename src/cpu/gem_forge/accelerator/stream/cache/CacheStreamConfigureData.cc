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