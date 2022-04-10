#ifndef __CPU_GEM_FORGE_MLC_STRAND_MANAGER_HH__
#define __CPU_GEM_FORGE_MLC_STRAND_MANAGER_HH__

#include "MLCStreamEngine.hh"

class MLCStrandManager {
public:
  MLCStrandManager(MLCStreamEngine *_mlcSE);
  ~MLCStrandManager();

  /**
   * Receive a StreamConfig message and configure all streams.
   */
  void receiveStreamConfigure(PacketPtr pkt);

  /**
   * Receive a StreamEnd message and end all streams.
   */
  void receiveStreamEnd(PacketPtr pkt);

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
  AbstractStreamAwareController *controller;

  using ConfigPtr = CacheStreamConfigureDataPtr;
  using ConfigVec = CacheStreamConfigureVec;

  std::unordered_map<DynStrandId, MLCDynStream *, DynStrandIdHasher> strandMap;

  /**
   * Check if streams can be sliced.
   */
  void checkShouldBeSliced(ConfigVec &configs) const;

  /**
   * @brief States during splitting streams into strands.
   */
  struct StrandSplitContext {
    int64_t noSplitOuterTripCount = 0;
    int totalStrands = 1;
    // We allow each stream to have specific interleaving.
    struct ContextPerStream {
      int splitDim = 0;
      int splitInterleave = 0;
    };
    std::map<DynStreamId, ContextPerStream> perStreamContext;
  };

  /**
   * Split stream into strands.
   */
  bool canSplitIntoStrands(StrandSplitContext &context,
                           const ConfigVec &configs) const;
  bool canSplitIntoStrands(StrandSplitContext &context, ConfigPtr config) const;
  void splitIntoStrands(StrandSplitContext &context, ConfigVec &configs);
  ConfigVec splitIntoStrands(StrandSplitContext &context, ConfigPtr config);
  ConfigVec splitIntoStrandsImpl(StrandSplitContext &context, ConfigPtr config,
                                 StrandSplitInfo &strandSplit, bool isDirect);
  DynStreamFormalParamV splitAffinePattern(StrandSplitContext &context,
                                           ConfigPtr config,
                                           const StrandSplitInfo &strandSplit,
                                           int strandIdx);

  /**
   * Configure a single stream.
   * It will insert an configure message into the message buffer to configure
   * the correct LLC bank.
   * In case the first element's virtual address faulted, the MLC StreamEngine
   * will return physical address that maps to the LLC bank of this tile.
   */
  void configureStream(ConfigPtr configs, MasterID masterId);

  /**
   * Send configure message to remote SE.
   */
  void sendConfigToRemoteSE(ConfigPtr streamConfigureData, MasterID masterId);
  /**
   * Receive a StreamEnd message.
   * The difference between StreamConfigure and StreamEnd message
   * is that the first one already knows the destination LLC bank from the core
   * StreamEngine, while the later one has to rely on the MLC StreamEngine as it
   * has the flow control information and knows where the stream is.
   * It will terminate the stream and send a EndPacket to the LLC bank.
   */
  void endStream(const DynStreamId &endId, MasterID masterId);
};

#endif