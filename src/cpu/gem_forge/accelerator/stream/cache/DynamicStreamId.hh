
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
  int coreId = -1;
  uint64_t staticId = 0;
  uint64_t streamInstance = 0;
  // Used for debug purpose. User should guarantee the life cycle of name.
  // TODO: How to improve this?
  const char *streamName = "Unknown_Stream";

  DynamicStreamId() = default;
  DynamicStreamId(int _coreId, uint64_t _staticId, uint64_t _streamInstance)
      : coreId(_coreId), staticId(_staticId), streamInstance(_streamInstance) {}
  DynamicStreamId(const DynamicStreamId &other)
      : coreId(other.coreId), staticId(other.staticId),
        streamInstance(other.streamInstance), streamName(other.streamName) {}
  DynamicStreamId &operator=(const DynamicStreamId &other) {
    this->coreId = other.coreId;
    this->staticId = other.staticId;
    this->streamInstance = other.streamInstance;
    this->streamName = other.streamName;
    return *this;
  }

  DynamicStreamId(DynamicStreamId &&other) = delete;
  DynamicStreamId &operator=(DynamicStreamId &&other) = delete;

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