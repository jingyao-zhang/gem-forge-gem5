#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_FLOAT_CONTROLLER_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_FLOAT_CONTROLLER_HH__

#include "stream_engine.hh"

/**
 * Helper class to manager the floating streams.
 * So far this is only in charge of floating streams at configuration.
 */
class StreamFloatController {
public:
  StreamFloatController(StreamEngine *_se,
                        std::unique_ptr<StreamFloatPolicy> _policy);

  using StreamConfigArgs = StreamEngine::StreamConfigArgs;
  void floatStreams(const StreamConfigArgs &args,
                    const ::LLVM::TDG::StreamRegion &streamRegion,
                    std::list<Stream *> &configStreams);

private:
  StreamEngine *se;
  std::unique_ptr<StreamFloatPolicy> policy;

  using StreamCacheConfigMap =
      std::unordered_map<Stream *, CacheStreamConfigureDataPtr>;
  void floatReductionStreams(const StreamConfigArgs &args,
                             const ::LLVM::TDG::StreamRegion &streamRegion,
                             std::list<Stream *> &configStreams,
                             StreamCacheConfigMap &floatedMap);
};

#endif