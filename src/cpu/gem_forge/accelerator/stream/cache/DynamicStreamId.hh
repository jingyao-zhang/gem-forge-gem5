
#ifndef __CPU_TDG_ACCELERATOR_DYNAMIC_STREAM_ID_HH__
#define __CPU_TDG_ACCELERATOR_DYNAMIC_STREAM_ID_HH__

#include <functional>
#include <iostream>

/**
 * Uniquely identifies a dynamic stream in the system.
 * I try to define it as independent as implementation of stream.
 */
struct DynamicStreamId {
  using StaticId = uint64_t;
  using InstanceId = uint64_t;
  static constexpr StaticId InvalidStaticStreamId = 0;
  static constexpr InstanceId InvalidInstanceId = 0;
  // Use coreId to distinguish streams in multi-core context.
  // TODO: ThreadID may be a better option.
  int coreId = -1;
  StaticId staticId = 0;
  InstanceId streamInstance = 0;
  // Used for debug purpose. User should guarantee the life cycle of name.
  // TODO: How to improve this?
  const char *streamName = "Unknown_Stream";

  DynamicStreamId() = default;
  DynamicStreamId(int _coreId, StaticId _staticId, InstanceId _streamInstance)
      : coreId(_coreId), staticId(_staticId), streamInstance(_streamInstance) {}
  DynamicStreamId(int _coreId, StaticId _staticId, InstanceId _streamInstance,
                  const char *_streamName)
      : coreId(_coreId), staticId(_staticId), streamInstance(_streamInstance),
        streamName(_streamName) {}
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

  bool isSameStaticStream(const DynamicStreamId &other) const {
    return this->coreId == other.coreId && this->staticId == other.staticId;
  }
  bool operator==(const DynamicStreamId &other) const {
    return this->coreId == other.coreId && this->staticId == other.staticId &&
           this->streamInstance == other.streamInstance;
  }
  bool operator!=(const DynamicStreamId &other) const {
    return !(this->operator==(other));
  }
  bool operator<(const DynamicStreamId &other) const {
    if (this->coreId != other.coreId) {
      return this->coreId < other.coreId;
    }
    if (this->staticId != other.staticId) {
      return this->staticId < other.staticId;
    }
    return this->streamInstance < other.streamInstance;
  }
};

std::ostream &operator<<(std::ostream &os, const DynamicStreamId &streamId);

std::string to_string(const DynamicStreamId &streamId);

struct DynamicStreamIdHasher {
  std::size_t operator()(const DynamicStreamId &key) const {
    return (std::hash<int>()(key.coreId)) ^
           (std::hash<DynamicStreamId::StaticId>()(key.staticId)) ^
           (std::hash<DynamicStreamId::InstanceId>()(key.streamInstance));
  }
};

#endif