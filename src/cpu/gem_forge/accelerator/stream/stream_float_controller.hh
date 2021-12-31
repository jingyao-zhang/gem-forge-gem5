#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_FLOAT_CONTROLLER_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_FLOAT_CONTROLLER_HH__

#include "stream_engine.hh"

#include <map>

/**
 * Helper class to manager the floating streams.
 * So far this is only in charge of floating streams at configuration.
 */
class StreamFloatController {
public:
  StreamFloatController(StreamEngine *_se,
                        std::unique_ptr<StreamFloatPolicy> _policy);

  using StreamList = std::list<Stream *>;
  using DynStreamList = std::list<DynamicStream *>;
  using DynStreamVec = std::vector<DynamicStream *>;

  using StreamConfigArgs = StreamEngine::StreamConfigArgs;
  void floatStreams(const StreamConfigArgs &args,
                    const ::LLVM::TDG::StreamRegion &region,
                    DynStreamList &dynStreams);

  void commitFloatStreams(const StreamConfigArgs &args,
                          const StreamList &streams);
  void rewindFloatStreams(const StreamConfigArgs &args,
                          const StreamList &streams);
  void endFloatStreams(const DynStreamVec &dynStreams);

  /**
   * Handle midway florat.
   */
  void processMidwayFloat();

private:
  StreamEngine *se;
  std::unique_ptr<StreamFloatPolicy> policy;

  /**
   * A internal structure to memorize the floating decision made so far.
   */
  using StreamCacheConfigMap = StreamFloatPolicy::StreamCacheConfigMap;

  struct Args {
    const ::LLVM::TDG::StreamRegion &region;
    InstSeqNum seqNum;
    DynStreamList &dynStreams;
    StreamCacheConfigMap &floatedMap;
    CacheStreamConfigureVec &rootConfigVec;
    Args(const ::LLVM::TDG::StreamRegion &_region, InstSeqNum _seqNum,
         DynStreamList &_dynStreams, StreamCacheConfigMap &_floatedMap,
         CacheStreamConfigureVec &_rootConfigVec)
        : region(_region), seqNum(_seqNum), dynStreams(_dynStreams),
          floatedMap(_floatedMap), rootConfigVec(_rootConfigVec) {}
  };

  void floatDirectLoadStreams(const Args &args);
  void floatDirectAtomicComputeStreams(const Args &args);
  void floatPointerChaseStreams(const Args &args);
  void floatIndirectStreams(const Args &args);
  void floatDirectStoreComputeOrUpdateStreams(const Args &args);
  void floatDirectOrPointerChaseReductionStreams(const Args &args);
  void floatIndirectReductionStreams(const Args &args);
  void floatIndirectReductionStream(const Args &args, DynamicStream *dynS);
  void floatTwoLevelIndirectStoreComputeStreams(const Args &args);
  void floatTwoLevelIndirectStoreComputeStream(const Args &args,
                                               DynamicStream *dynS);

  /**
   * If the loop is eliminated, we mark some addition fields in the
   * configuration.
   */
  void floatEliminatedLoop(const Args &args);

  /**
   * For now we can rewind a floated stream that write to memory (Store/Atomic
   * Compute Stream). As a temporary fix, I delay sending out the floating
   * packet until the StreamConfig is committed, and raise the "offloadDelayed"
   * flag in the DynamicStream -- which will stop the StreamEngine issuing them.
   */
  using SeqNumToPktMapT = std::map<InstSeqNum, PacketPtr>;
  using SeqNumToPktMapIter = SeqNumToPktMapT::iterator;
  SeqNumToPktMapT configSeqNumToDelayedFloatPktMap;

  /**
   * These are ConfigPkts delayed to wait for FirstFloatedElemIdx.
   * Their StreamConfigs are committed.
   */
  SeqNumToPktMapT configSeqNumToMidwayFloatPktMap;

  /**
   * Check if there is an aliased StoreStream for this LoadStream, but
   * is not promoted into an UpdateStream.
   */
  bool checkAliasedUnpromotedStoreStream(DynamicStream *dynS);

  /**
   * Determine the FirstOffloadedElementIdx.
   * Mainly used to optimize for pointer-chase stream.
   */
  void setFirstOffloadedElementIdx(const Args &args);

  /**
   * Propagate FloatPlan to ConfigureData.
   */
  void propagateFloatPlan(const Args &args);

  /**
   * Try send out a midway float pkt.
   */
  bool trySendMidwayFloat(SeqNumToPktMapIter iter);

  /**
   * Check if a MidwayFloat is ready to issue.
   */
  bool isMidwayFloatReady(CacheStreamConfigureDataPtr &config);
};

#endif