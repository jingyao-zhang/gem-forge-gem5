#include "CacheStreamConfigureData.hh"

CacheStreamConfigureData::CacheStreamConfigureData(
    Stream *_stream, const DynamicStreamId &_dynamicId, int _elementSize,
    HistoryPtr _history)
    : stream(_stream), dynamicId(_dynamicId), elementSize(_elementSize),
      history(_history), initVAddr(0), initPAddr(0), isPointerChase(false),
      indirectStreamConfigure(nullptr), initAllocatedIdx(0) {
  assert(this->history->history_size() > 0 && "Empty stream?");

  const auto &entry = this->history->history(0);
  this->initVAddr = entry.addr();
}

CacheStreamConfigureData::CacheStreamConfigureData(
    const CacheStreamConfigureData &other)
    : stream(other.stream), dynamicId(other.dynamicId), history(other.history),
      initVAddr(other.initVAddr), initPAddr(other.initPAddr),
      isPointerChase(other.isPointerChase),
      indirectStreamConfigure(other.indirectStreamConfigure),
      initAllocatedIdx(other.initAllocatedIdx) {}