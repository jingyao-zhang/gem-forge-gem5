
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

  /*******************************************************************************
   * Static information accessor.
   *******************************************************************************/
  ::LLVM::TDG::StreamInfo_Type getStreamType() const override;

  uint32_t getLoopLevel() const override { return this->info.loop_level(); }

  uint32_t getConfigLoopLevel() const override {
    return this->info.config_loop_level();
  }

  int32_t getElementSize() const override;

  bool getFloatManual() const override {
    return this->info.static_info().float_manual();
  }

  bool hasUpdate() const override {
    return this->info.static_info().has_update();
  }
  bool hasUpgradedToUpdate() const override {
    return this->info.static_info().has_upgraded_to_update();
  }

  const PredicatedStreamIdList &getMergedPredicatedStreams() const override {
    return this->info.static_info().merged_predicated_streams();
  }

  const ::LLVM::TDG::ExecFuncInfo &getPredicateFuncInfo() const override {
    return this->info.static_info().pred_func_info();
  }

  const StreamIdList &getMergedLoadStoreDepStreams() const override {
    return this->info.static_info().merged_load_store_dep_streams();
  }
  const StreamIdList &getMergedLoadStoreBaseStreams() const override {
    return this->info.static_info().merged_load_store_base_streams();
  }
  const ::LLVM::TDG::ExecFuncInfo &getStoreFuncInfo() const override {
    return this->info.static_info().store_func_info();
  }

  bool isMergedPredicated() const override {
    return this->info.static_info().is_merged_predicated_stream();
  }
  bool isMergedLoadStoreDepStream() const override {
    return this->info.static_info().merged_load_store_base_streams_size() > 0;
  }
  bool enabledStoreFunc() const override {
    return this->info.static_info().enabled_store_func();
  }

  const ::LLVM::TDG::StreamParam &getConstUpdateParam() const override {
    return this->info.static_info().const_update_param();
  }

  bool isReduction() const override {
    return this->info.static_info().val_pattern() ==
           ::LLVM::TDG::StreamValuePattern::REDUCTION;
  }

  bool hasCoreUser() const override {
    return !this->info.static_info().no_core_user();
  }

  bool isContinuous() const override;
  void configure(uint64_t seqNum, ThreadContext *tc) override;

  uint64_t getTrueFootprint() const override;
  uint64_t getFootprint(unsigned cacheBlockSize) const override;

  void setupAddrGen(DynamicStream &dynStream,
                    const InputVecT *inputVec) override;

  bool isPointerChaseLoadStream() const override;
  uint64_t getStreamLengthAtInstance(uint64_t streamInstance) const override;

private:
  LLVM::TDG::StreamInfo info;
  std::unique_ptr<StreamHistory> history;
  std::unique_ptr<StreamPattern> patternStream;

  void initializeBaseStreams();
  void initializeBackBaseStreams();
  void initializeAliasStreams();
};

#endif