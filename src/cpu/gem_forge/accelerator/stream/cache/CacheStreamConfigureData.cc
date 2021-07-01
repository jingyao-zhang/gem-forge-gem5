#include "CacheStreamConfigureData.hh"

#include "debug/StreamLoopBound.hh"
#define DEBUG_TYPE StreamLoopBound
#include "../stream_log.hh"

CacheStreamConfigureData::CacheStreamConfigureData(
    Stream *_stream, const DynamicStreamId &_dynamicId, int _elementSize,
    const std::vector<DynamicStreamFormalParam> &_addrGenFormalParams,
    std::shared_ptr<AddrGenCallback> _addrGenCallback)
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