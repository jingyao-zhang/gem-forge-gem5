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

  using StreamConfigArgs = StreamEngine::StreamConfigArgs;
  void floatStreams(const StreamConfigArgs &args,
                    const ::LLVM::TDG::StreamRegion &region,
                    DynStreamList &dynStreams);

  void commitFloatStreams(const StreamConfigArgs &args,
                          const StreamList &streams);
  void rewindFloatStreams(const StreamConfigArgs &args,
                          const StreamList &streams);

private:
  StreamEngine *se;
  std::unique_ptr<StreamFloatPolicy> policy;

  /**
   * A internal structure to memorize the floating decision made so far.
   */
  using StreamCacheConfigMap =
      std::unordered_map<Stream *, CacheStreamConfigureDataPtr>;

  struct Args {
    const ::LLVM::TDG::StreamRegion &region;
    DynStreamList &dynStreams;
    StreamCacheConfigMap &floatedMap;
    CacheStreamConfigureVec &rootConfigVec;
    Args(const ::LLVM::TDG::StreamRegion &_region, DynStreamList &_dynStreams,
         StreamCacheConfigMap &_floatedMap,
         CacheStreamConfigureVec &_rootConfigVec)
        : region(_region), dynStreams(_dynStreams), floatedMap(_floatedMap),
          rootConfigVec(_rootConfigVec) {}
  };

  void floatDirectLoadStreams(const Args &args);
  void floatDirectAtomicComputeStreams(const Args &args);
  void floatIndirectStreams(const Args &args);
  void floatDirectStoreComputeOrUpdateStreams(const Args &args);
  void floatDirectReductionStreams(const Args &args);
  void floatIndirectReductionStreams(const Args &args);
  void floatIndirectReductionStream(const Args &args, DynamicStream *dynS);

  /**
   * For now we can rewind a floated stream that write to memory (Store/Atomic
   * Compute Stream). As a temporary fix, I delay sending out the floating
   * packet until the StreamConfig is committed, and raise the "offloadDelayed"
   * flag in the DynamicStream -- which will stop the StreamEngine issuing them.
   */
  std::map<InstSeqNum, PacketPtr> configSeqNumToDelayedFloatPktMap;
};

#endif