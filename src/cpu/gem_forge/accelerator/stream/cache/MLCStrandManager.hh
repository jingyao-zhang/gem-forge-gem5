#ifndef __CPU_GEM_FORGE_MLC_STRAND_MANAGER_HH__
#define __CPU_GEM_FORGE_MLC_STRAND_MANAGER_HH__

#include "MLCStreamEngine.hh"
#include "StreamReuseAnalyzer.hh"

namespace gem5 {

class MLCStrandManager {
public:
  using Config = CacheStreamConfigureData;
  using ConfigPtr = CacheStreamConfigureDataPtr;
  using ConfigVec = CacheStreamConfigureVec;
  using StreamToStrandsMap = std::unordered_map<ConfigPtr, ConfigVec>;

  MLCStrandManager(MLCStreamEngine *_mlcSE);
  ~MLCStrandManager();

  /**
   * Receive a StreamConfig message and configure all streams.
   */
  void receiveStreamConfigure(ConfigVec *configs, RequestorID requestorId);

  /**
   * Receive a StreamEnd message and end all streams.
   */
  void receiveStreamEnd(const std::vector<DynStreamId> &endIds,
                        RequestorID requestorId);

  /**
   * Function called to check CoreCommitProgress for RangeSync.
   */
  void checkCoreCommitProgress();

  /**
   * Get CoreSE. Only valid when there are streams configured.
   */
  StreamEngine *getCoreSE() const;

  /**
   * API to get the MLCDynStream.
   */
  MLCDynStream *getStreamFromStrandId(const DynStrandId &id);

  /**
   * API to get the strand from core slice id.
   */
  MLCDynStream *getStreamFromCoreSliceId(const DynStreamSliceId &sliceId);

  bool hasConfiguredStreams() const { return !this->strandMap.empty(); }

  /**
   * Check the progress of certain stream element is acked.
   */
  bool isStreamElemAcked(const DynStreamId &streamId, uint64_t streamElemIdx,
                         MLCDynStream::ElementCallback callback);

private:
  MLCStreamEngine *mlcSE;
  ruby::AbstractStreamAwareController *controller;
  std::unique_ptr<StreamReuseAnalyzer> reuseAnalyzer;

  std::unordered_map<DynStrandId, MLCDynStream *, DynStrandIdHasher> strandMap;

  /**
   * Check if streams can be sliced.
   */
  void checkShouldBeSliced(ConfigVec &configs) const;

  /**
   * @brief States during splitting streams into strands.
   */
  struct StrandSplitContext {
    int64_t noSplitOuterTrip = 0;
    int totalStrands = 1;
    // Whether we want to skip sanity check.
    bool skipSanityCheck = false;
    // We allow each stream to have specific interleaving.
    struct ContextPerStream {
      // We allow split some continguous dimensions.
      // Ordered from inner-most dimension (dim 0).
      using SplitDim = StrandSplitInfo::SplitDim;
      std::vector<SplitDim> splitDims;
      int64_t innerTrip = 0;
      int64_t outerTrip = 0;
      int64_t splitTripPerStrand = 0;
      // int64_t splitTailInterleave = 0;
      std::vector<int64_t> trips;
      std::vector<int64_t> strides;
    };
    /**
     * Fields used for split by Elem.
     */
    bool splitByElem = false;
    StrandSplitInfo splitByElemInfo;
    std::map<DynStreamId, ContextPerStream> perStreamContext;
  };

  /**
   * Split stream into strands.
   */
  bool canSplitIntoStrands(StrandSplitContext &context,
                           const ConfigVec &configs) const;
  bool canSplitIntoStrandsByElem(StrandSplitContext &context,
                                 const ConfigVec &configs) const;
  bool chooseNoSplitOuterTrip(StrandSplitContext &context,
                              const ConfigVec &configs) const;
  bool precheckSplitable(StrandSplitContext &context, ConfigPtr config) const;
  /**
   * Try reduce SplitDimIntrlv to avoid starting all strands at the same bank.
   */
  void tryAvoidStartStrandsAtSameBank(ConfigPtr config, const int llcBankIntrlv,
                                      const int64_t splitDimStride,
                                      int64_t &splitDimTripPerStrand) const;
  bool chooseSplitDimIntrlv(StrandSplitContext &context,
                            const ConfigVec &configs) const;
  bool chooseSplitDimIntrlvMMOuter(StrandSplitContext &context,
                                   const ConfigVec &configs) const;
  bool chooseSplitDimIntrlvByUser(StrandSplitContext &context,
                                  const ConfigVec &configs) const;
  bool chooseSplitDimIntrlv(StrandSplitContext &context,
                            ConfigPtr config) const;
  bool fixSplitDimIntrlv(StrandSplitContext &context,
                         const ConfigVec &configs) const;
  bool postcheckSplitable(StrandSplitContext &context, ConfigPtr config) const;
  void splitIntoStrands(StrandSplitContext &context, ConfigVec &configs);
  ConfigVec splitIntoStrands(StrandSplitContext &context, ConfigPtr config);
  ConfigVec splitIntoStrandsImpl(StrandSplitContext &context, ConfigPtr config,
                                 StrandSplitInfo strandSplit, bool isDirect);
  DynStreamFormalParamV splitAffinePattern(StrandSplitContext &context,
                                           ConfigPtr config,
                                           const StrandSplitInfo &strandSplit,
                                           int strandIdx);
  /**
   * Split the affine pattern by dimensions.
   */
  DynStreamFormalParamV
  splitAffinePatternByDim(StrandSplitContext &context, ConfigPtr config,
                          const StrandSplitInfo &strandSplit, int strandIdx);
  /**
   * Split the affine pattern by one dimension.
   */
  DynStreamFormalParamV
  splitAffinePatternAtDim(const DynStreamId &dynId,
                          const DynStreamFormalParamV &params, int splitDim,
                          int64_t splitIntrlv, int splitCnt, int strandIdx);
  void fixReusedSendTo(StrandSplitContext &context, ConfigVec &streamConfigs,
                       ConfigVec &strandConfigs);
  int64_t computeSplitInnerTrip(StrandSplitContext &context,
                                const ConfigPtr &config) const;

  /**
   * Recognize the broadcast opportunities between
   * strands.
   */
  void mergeBroadcastStrands(StrandSplitContext &context,
                             StreamToStrandsMap &streamToStrandMap,
                             CacheStreamConfigureVec &strands);

  /**
   * Recognize reuse with distance more than 1.
   */
  void recognizeReusedTile(StrandSplitContext &context, ConfigPtr strand);

  /**
   * Configure a single stream.
   * It will insert an configure message into the
   * message buffer to configure the correct LLC bank.
   * In case the first element's virtual address
   * faulted, the MLC StreamEngine will return physical
   * address that maps to the LLC bank of this tile.
   */
  void configureStream(ConfigPtr configs, RequestorID requestorId);

  /**
   * Send configure message to remote SE.
   */
  void sendConfigToRemoteSE(ConfigPtr streamConfigureData,
                            RequestorID requestorId);
  /**
   * Mark a PUMRegion cached when a stream accessed the
   * entire region.
   */
  void tryMarkPUMRegionCached(const DynStreamId &dynId);
  /**
   * Receive a StreamEnd message.
   * The difference between StreamConfigure and
   * StreamEnd message is that the first one already
   * knows the destination LLC bank from the core
   * StreamEngine, while the later one has to rely on
   * the MLC StreamEngine as it has the flow control
   * information and knows where the stream is. It will
   * terminate the stream and send a EndPacket to the
   * LLC bank.
   */
  void endStream(const DynStreamId &endId, RequestorID requestorId);
};

} // namespace gem5

#endif
