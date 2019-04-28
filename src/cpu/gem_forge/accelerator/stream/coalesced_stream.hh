#ifndef __CPU_TDG_ACCELERATOR_COALESCED_STREAM_HH__
#define __CPU_TDG_ACCELERATOR_COALESCED_STREAM_HH__

#include "stream.hh"

#include "stream_history.hh"
#include "stream_pattern.hh"

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif

#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"

class StreamInst;

/**
 * A simple logical stream managed by the coalesced stream.
 */
class LogicalStream {
public:
  LogicalStream(const std::string &_traceExtraFolder,
                const LLVM::TDG::StreamInfo &_info);

  LogicalStream(const LogicalStream &Other) = delete;
  LogicalStream(LogicalStream &&Other) = delete;
  LogicalStream &operator=(const LogicalStream &Other) = delete;
  LogicalStream &operator=(LogicalStream &&Other) = delete;

  ~LogicalStream();

  LLVM::TDG::StreamInfo info;
  std::unique_ptr<StreamHistory> history;
  std::unique_ptr<StreamPattern> patternStream;
};

class CoalescedStream : public Stream {
public:
  CoalescedStream(LLVMTraceCPU *_cpu, StreamEngine *_se,
                  const LLVM::TDG::StreamInfo &_primaryInfo, bool _isOracle,
                  size_t _maxRunAHeadLength);

  ~CoalescedStream();

  void addStreamInfo(const LLVM::TDG::StreamInfo &info);
  void prepareNewElement(StreamElement *element) override;

  uint64_t getCoalesceStreamId() const {
    return this->primaryLogicalStream->info.id();
  }

  /**
   * Only to configure all the history.
   */
  void configure(StreamConfigInst *inst) override;

  const std::string &getStreamName() const override;
  const std::string &getStreamType() const override;
  uint32_t getLoopLevel() const override;
  uint32_t getConfigLoopLevel() const override;
  int32_t getElementSize() const override;

  /**
   * Get the number of unique cache blocks the stream touches.
   * Used for stream aware cache to determine if it should cache the stream.
   */
  uint64_t getFootprint(unsigned cacheBlockSize) const override;
  uint64_t getTrueFootprint() const override;

  bool isContinuous() const override;

protected:
  /**
   * Represented all the streams coalesced within this one.
   * The first one is "primary stream", whose stream id is used to represent
   * this coalesced stream.
   */
  std::list<LogicalStream> coalescedStreams;
  LogicalStream *primaryLogicalStream;

  std::string streamName;
  void generateStreamName();

  // bool shouldHandleStreamInst(StreamInst *inst) const;

  void handlePacketResponse(const FIFOEntryIdx &entryId, PacketPtr packet,
                            StreamMemAccess *memAccess) override;

  /**
   * Merge the request from different logical streams.
   */
  // std::pair<uint64_t, uint64_t> getNextAddr();

  /**
   * For debug.
   */
  void dump() const override;
};

#endif