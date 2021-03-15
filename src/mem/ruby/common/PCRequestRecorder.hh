#ifndef __MEM_RUBY_COMMON_PC_REQUEST_RECORDER_HH__
#define __MEM_RUBY_COMMON_PC_REQUEST_RECORDER_HH__

#include "base/types.hh"
#include "mem/ruby/protocol/RubyRequestType.hh"

#include <unordered_set>

class PCRequestRecorder {
public:
  PCRequestRecorder(const std::string &_name) : name(_name) {}

  void recordReq(Addr pc, RubyRequestType type, bool isStream,
                 const char *streamName, Cycles latency);

  void reset();
  void dump();

private:
  std::string name;
  std::ostream *pcLatencyStream = nullptr;

  //! Stats for recording latency by PC.
  struct RequestLatencyStats {
    RequestLatencyStats(Addr _pc, RubyRequestType _type, bool _isStream,
                        const char *_streamName)
        : pc(_pc), type(_type), isStream(_isStream), streamName(_streamName) {}
    const Addr pc;
    const RubyRequestType type;
    const bool isStream;
    const char *streamName = nullptr;
    mutable uint64_t totalReqs = 0;
    mutable uint64_t totalLatency = 0;
    bool operator==(const RequestLatencyStats &other) const {
      return pc == other.pc && type == other.type && isStream == other.isStream;
    }
    bool operator!=(const RequestLatencyStats &other) const {
      return !(this->operator==(other));
    }
    bool operator<(const RequestLatencyStats &other) const {
      if (pc != other.pc) {
        return pc < other.pc;
      }
      if (type != other.type) {
        return type < other.type;
      }
      return isStream < other.isStream;
    }
  };

  struct RequestLatencyStatsHasher {
    std::size_t operator()(const RequestLatencyStats &key) const {
      return (std::hash<int>()(key.type)) ^ (std::hash<uint64_t>()(key.pc)) ^
             (std::hash<bool>()(key.isStream));
    }
  };
  std::unordered_set<RequestLatencyStats, RequestLatencyStatsHasher>
      pcLatencySet;
};

#endif