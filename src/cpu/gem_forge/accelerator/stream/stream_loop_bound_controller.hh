#ifndef __CPU_GEM_FORGE_STREAM_LOOP_BOUND_CONTROLLER_H__
#define __CPU_GEM_FORGE_STREAM_LOOP_BOUND_CONTROLLER_H__

#include "stream_engine.hh"

class StreamLoopBoundController {
public:
  StreamLoopBoundController(StreamEngine *_se);
  ~StreamLoopBoundController();

  void initializeLoopBound(const ::LLVM::TDG::StreamRegion &region);

  using ConfigArgs = StreamEngine::StreamConfigArgs;
  using EndArgs = StreamEngine::StreamEndArgs;

  void dispatchStreamConfig(const ConfigArgs &args);
  void executeStreamConfig(const ConfigArgs &args);
  void rewindStreamConfig(const ConfigArgs &args);
  void commitStreamEnd(const EndArgs &args);

private:
  StreamEngine *se;

  struct StaticLoopBound;
  struct DynLoopBound {
    const StaticLoopBound *staticLoopBound;
    const uint64_t seqNum;
    ExecFuncPtr boundFunc;
    bool configExecuted = false;
    DynamicStreamFormalParamV formalParams;
    uint64_t nextElementIdx = 0;
    DynLoopBound(StaticLoopBound *_staticLoopBound, uint64_t _seqNum,
                 ExecFuncPtr _boundFunc)
        : staticLoopBound(_staticLoopBound), seqNum(_seqNum),
          boundFunc(_boundFunc) {}
  };

  std::map<uint64_t, DynLoopBound *> activeDynBoundMap;

  struct StaticLoopBound {
    const ::LLVM::TDG::StreamRegion &region;
    ExecFuncPtr boundFunc;
    bool boundRet;
    std::list<DynLoopBound> dynBounds;
    std::unordered_set<Stream *> baseStreams;
    StaticLoopBound(const ::LLVM::TDG::StreamRegion &_region,
                    ExecFuncPtr _boundFunc, bool _boundRet)
        : region(_region), boundFunc(_boundFunc), boundRet(_boundRet) {}
  };

  /**
   * Remember all static loop bound.
   */
  std::unordered_map<std::string, StaticLoopBound> staticBoundMap;
};

#endif