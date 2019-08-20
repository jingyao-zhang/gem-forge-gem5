#include "CacheStreamConfigureData.hh"

CacheStreamConfigureData::CacheStreamConfigureData(
    Stream *_stream, const DynamicStreamId &_dynamicId, HistoryPtr _history)
    : stream(_stream), dynamicId(_dynamicId), elementSize(0), history(_history),
      initVAddr(0), initPAddr(0), indirectStream(nullptr), initAllocatedIdx(0) {
  assert(this->history->history_size() > 0 && "Empty stream?");

  const auto &entry = this->history->history(0);
  this->initVAddr = entry.addr();

  // Set the element size.
  this->elementSize = this->stream->getElementSize();
}

CacheStreamConfigureData::CacheStreamConfigureData(
    const CacheStreamConfigureData &other)
    : stream(other.stream), dynamicId(other.dynamicId), history(other.history),
      initVAddr(other.initVAddr), initPAddr(other.initPAddr),
      indirectStream(other.indirectStream),
      indirectDynamicId(other.indirectDynamicId),
      indirectHistory(other.indirectHistory),
      initAllocatedIdx(other.initAllocatedIdx) {}