
#ifndef __CPU_TDG_ACCELERATOR_DYNAMIC_STREAM_ID_HH__
#define __CPU_TDG_ACCELERATOR_DYNAMIC_STREAM_ID_HH__

#include <functional>
#include <iostream>

/**
 * Uniquely identifies a dynamic stream in the system.
 * I try to define it as independent as implementation of stream.
 */
struct DynamicStreamId {
  // Use coreId to distinguish streams in multi-core context.
  // TODO: ThreadID may be a better option.
  int coreId;
  uint64_t staticId;
  uint64_t streamInstance;
  // Used for debug purpose.
  std::string name;

  bool operator==(const DynamicStreamId &other) const {
    return this->coreId == other.coreId && this->staticId == other.staticId &&
           this->streamInstance == other.streamInstance;
  }
  bool operator!=(const DynamicStreamId &other) const {
    return !(this->operator==(other));
  }
};

std::ostream &operator<<(std::ostream &os, const DynamicStreamId &streamId);

struct DynamicStreamIdHasher {
  std::size_t operator()(const DynamicStreamId &key) const {
    return (std::hash<int>()(key.coreId)) ^
           (std::hash<uint64_t>()(key.staticId)) ^
           (std::hash<uint64_t>()(key.streamInstance));
  }
};

#endif