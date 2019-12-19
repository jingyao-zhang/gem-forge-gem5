
#ifndef __CPU_TDG_ACCELERATOR_SINGLE_STREAM_HH__
#define __CPU_TDG_ACCELERATOR_SINGLE_STREAM_HH__

#include "stream.hh"

#include "stream_history.hh"
#include "stream_pattern.hh"

class SingleStream : public Stream {
public:
  SingleStream(const StreamArguments &args, const LLVM::TDG::StreamInfo &_info);

  ~SingleStream();

  void finalize() override;

  const std::string &getStreamType() const override;
  uint32_t getLoopLevel() const override;
  uint32_t getConfigLoopLevel() const override;
  int32_t getElementSize() const override;
  bool getFloatManual() const override;

  bool isContinuous() const override;
  void configure(uint64_t seqNum, ThreadContext *tc) override;

  uint64_t getTrueFootprint() const override;
  uint64_t getFootprint(unsigned cacheBlockSize) const override;

  void setupAddrGen(DynamicStream &dynStream,
                    const std::vector<uint64_t> *inputVec) override;

  bool isPointerChaseLoadStream() const override;
  uint64_t getStreamLengthAtInstance(uint64_t streamInstance) const override;

private:
  LLVM::TDG::StreamInfo info;
  std::unique_ptr<StreamHistory> history;
  std::unique_ptr<StreamPattern> patternStream;

  void initializeBaseStreams();
  void initializeBackBaseStreams();
};

#endif