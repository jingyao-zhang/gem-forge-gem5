#include "CacheStreamConfigureData.hh"

CacheStreamConfigureData::CacheStreamConfigureData(Stream *_stream,
                                                   HistoryPtr _history)
    : stream(_stream), history(_history), initVAddr(0), initPAddr(0),
      initAllocatedIdx(0) {
  assert(this->history->history_size() > 0 && "Empty stream?");

  const auto &entry = this->history->history(0);
  this->initVAddr = entry.addr();
}

CacheStreamConfigureData::CacheStreamConfigureData(
    const CacheStreamConfigureData &other)
    : stream(other.stream), history(other.history), initVAddr(other.initVAddr),
      initPAddr(other.initPAddr), initAllocatedIdx(other.initAllocatedIdx) {}