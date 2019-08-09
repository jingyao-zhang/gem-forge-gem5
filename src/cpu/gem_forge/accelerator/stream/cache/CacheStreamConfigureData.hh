#ifndef __CPU_TDG_ACCELERATOR_STREAM_CACHE_STREAM_CONFIGURE_DATA_H__
#define __CPU_TDG_ACCELERATOR_STREAM_CACHE_STREAM_CONFIGURE_DATA_H__

#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"

#include "base/types.hh"

#include <memory>

class Stream;

struct CacheStreamConfigureData {
public:
  using HistoryPtr = std::shared_ptr<::LLVM::TDG::StreamHistory>;
  CacheStreamConfigureData(Stream *_stream, HistoryPtr _history);
  CacheStreamConfigureData(const CacheStreamConfigureData &other);

  Stream *stream;
  HistoryPtr history;
  Addr initVAddr;
  Addr initPAddr;
  // Set by the MLC stream, for flow control.
  int initAllocatedIdx;
};

#endif