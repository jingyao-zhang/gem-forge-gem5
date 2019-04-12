
#ifndef __CPU_TDG_ACCELERATOR_SINGLE_STREAM_HH__
#define __CPU_TDG_ACCELERATOR_SINGLE_STREAM_HH__

#include "stream_history.hh"

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif

#include "cpu/llvm_trace/accelerator/stream/StreamMessage.pb.h"
#include "stream.hh"

class SingleStream : public Stream {
 public:
  SingleStream(LLVMTraceCPU *_cpu, StreamEngine *_se,
               const LLVM::TDG::StreamInfo &_info, bool _isOracle,
               size_t _maxRunAHeadLength, const std::string &_throttling);

  ~SingleStream();

  const std::string &getStreamName() const override;
  const std::string &getStreamType() const override;
  uint32_t getLoopLevel() const override;
  uint32_t getConfigLoopLevel() const override;
  int32_t getElementSize() const override;

  void prepareNewElement(StreamElement *element) override;

  bool isContinuous() const override;
  void configure(StreamConfigInst *inst) override;

  uint64_t getTrueFootprint() const override;
  uint64_t getFootprint(unsigned cacheBlockSize) const override;

 private:
  LLVM::TDG::StreamInfo info;
  std::unique_ptr<StreamHistory> history;
  void handlePacketResponse(const FIFOEntryIdx &entryId, PacketPtr packet,
                            StreamMemAccess *memAccess) override;

  /**
   * For debug.
   */
  void dump() const override;
};

#endif