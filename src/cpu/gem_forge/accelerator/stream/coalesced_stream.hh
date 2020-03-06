#ifndef __CPU_TDG_ACCELERATOR_COALESCED_STREAM_HH__
#define __CPU_TDG_ACCELERATOR_COALESCED_STREAM_HH__

#include "stream.hh"

#include "stream_history.hh"
#include "stream_pattern.hh"

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

  uint64_t getCoalesceBaseStreamId() const {
    return this->info.coalesce_info().base_stream();
  }
  int32_t getCoalesceOffset() const {
    return this->info.coalesce_info().offset();
  }
  int32_t getElementSize() const { return this->info.element_size(); }
  uint64_t getStreamId() const { return this->info.id(); }
  const Stream::StreamIdList &getMergedLoadStoreDepStreams() const {
    return this->info.static_info().merged_load_store_dep_streams();
  }
  const Stream::StreamIdList &getMergedLoadStoreBaseStreams() const {
    return this->info.static_info().merged_load_store_base_streams();
  }
  const ::LLVM::TDG::ExecFuncInfo &getStoreFuncInfo() const {
    return this->info.static_info().store_func_info();
  }

  LLVM::TDG::StreamInfo info;
  std::unique_ptr<StreamHistory> history;
  std::unique_ptr<StreamPattern> patternStream;
};

class CoalescedStream : public Stream {
public:
  CoalescedStream(const StreamArguments &args, bool _staticCoalesced);

  ~CoalescedStream();

  void addStreamInfo(const LLVM::TDG::StreamInfo &info);
  void finalize() override;

  /**
   * Only to configure all the history.
   */
  void configure(uint64_t seqNum, ThreadContext *tc) override;

  /*******************************************************************************
   * Static information accessor.
   *******************************************************************************/
  const std::string &getStreamType() const override;
  uint32_t getLoopLevel() const override;
  uint32_t getConfigLoopLevel() const override;
  int32_t getElementSize() const override {
    assert(this->coalescedElementSize > 0 && "Invalid element size.");
    return this->coalescedElementSize;
  }
  bool getFloatManual() const override;
  bool hasUpgradedToUpdate() const override;
  const PredicatedStreamIdList &getMergedPredicatedStreams() const override;
  const ::LLVM::TDG::ExecFuncInfo &getPredicateFuncInfo() const override;

  const StreamIdList &getMergedLoadStoreDepStreams() const override {
    return this->primeLStream->getMergedLoadStoreDepStreams();
  }
  const StreamIdList &getMergedLoadStoreBaseStreams() const override {
    return this->primeLStream->getMergedLoadStoreBaseStreams();
  }
  const ::LLVM::TDG::ExecFuncInfo &getStoreFuncInfo() const override {
    return this->primeLStream->getStoreFuncInfo();
  }

  bool isMergedPredicated() const override;
  bool isMergedLoadStoreDepStream() const override;
  const ::LLVM::TDG::StreamParam &getConstUpdateParam() const override;
  bool isReduction() const override;
  bool hasCoreUser() const override;

  uint64_t getCoalesceBaseStreamId() const override {
    return this->primeLStream->getCoalesceBaseStreamId();
  }
  int32_t getCoalesceOffset() const override {
    // This is the true offset.
    return this->primeLStream->getCoalesceOffset();
  }
  size_t getNumCoalescedStreams() const {
    return this->coalescedStreams.size();
  }

  /**
   * Get the number of unique cache blocks the stream touches.
   * Used for stream aware cache to determine if it should cache the stream.
   */
  uint64_t getFootprint(unsigned cacheBlockSize) const override;
  uint64_t getTrueFootprint() const override;

  bool isContinuous() const override;

  void setupAddrGen(DynamicStream &dynStream,
                    const std::vector<uint64_t> *inputVec) override;

  uint64_t getStreamLengthAtInstance(uint64_t streamInstance) const override;

  void getCoalescedOffsetAndSize(uint64_t streamId, int32_t &offset,
                                 int32_t &size) const;

protected:
  /**
   * Represented all the streams coalesced within this one.
   * The first one is "prime stream", whose stream id is used to represent
   * this coalesced stream.
   * In statically coalesced streams, this is the base stream with offset 0.
   */
  bool staticCoalesced;
  std::vector<LogicalStream *> coalescedStreams;
  LogicalStream *primeLStream;
  int32_t coalescedElementSize = -1;
  int32_t baseOffset = -1;

  void selectPrimeLogicalStream();
  void initializeBaseStreams();
  void initializeBackBaseStreams();
};

#endif