
#ifndef __CPU_TDG_ACCELERATOR_SINGLE_STREAM_HH__
#define __CPU_TDG_ACCELERATOR_SINGLE_STREAM_HH__

#include "stream.hh"

#include "stream_history.hh"
#include "stream_pattern.hh"

class SingleStream : public Stream {
public:
  SingleStream(const StreamArguments &args, const LLVM::TDG::StreamInfo &_info);

  ~SingleStream();

  const std::string &getStreamName() const override;
  const std::string &getStreamType() const override;
  uint32_t getLoopLevel() const override;
  uint32_t getConfigLoopLevel() const override;
  int32_t getElementSize() const override;

  void initializeBackBaseStreams() override;
  void prepareNewElement(StreamElement *element) override;

  bool isContinuous() const override;
  void configure(StreamConfigInst *inst) override;

  uint64_t getTrueFootprint() const override;
  uint64_t getFootprint(unsigned cacheBlockSize) const override;

  /**
   * ! Sean: StreamAwareCache
   * Allocate the CacheStreamConfigureData.
   */
  CacheStreamConfigureData *
  allocateCacheConfigureData(uint64_t configSeqNum) override;
  bool isDirectLoadStream() const override;
  bool isPointerChaseLoadStream() const override;

private:
  LLVM::TDG::StreamInfo info;
  std::unique_ptr<StreamHistory> history;
  std::unique_ptr<StreamPattern> patternStream;

  /**
   * For debug.
   */
  void dump() const override;
};

#endif