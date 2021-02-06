
#ifndef __CPU_GEM_FORGE_ACCELERATOR_NEST_STREAM_CONTROLLER_H__
#define __CPU_GEM_FORGE_ACCELERATOR_NEST_STREAM_CONTROLLER_H__

#include "stream_engine.hh"

class NestStreamController {
public:
  NestStreamController(StreamEngine *_se);
  ~NestStreamController();

  void initializeNestConfig(const ::LLVM::TDG::StreamRegion &region);

  using ConfigArgs = StreamEngine::StreamConfigArgs;
  using EndArgs = StreamEngine::StreamEndArgs;
  void dispatchStreamConfig(const ConfigArgs &args);
  void executeStreamConfig(const ConfigArgs &args);
  void rewindStreamConfig(const ConfigArgs &args);
  void commitStreamEnd(const EndArgs &args);

  void configureNestStreams();

  void takeOverBy(GemForgeCPUDelegator *newCPUDelegator);

private:
  StreamEngine *se;
  GemForgeISAHandler isaHandler;

  struct StaticNestConfig;
  struct DynNestConfig {
    const StaticNestConfig *staticNestConfig;
    const uint64_t seqNum;
    ExecFuncPtr configFunc;
    bool configExecuted = false;
    DynamicStreamFormalParamV formalParams;
    uint64_t nextElementIdx = 0;
    DynNestConfig(StaticNestConfig *_staticNestConfig, uint64_t _seqNum,
                  ExecFuncPtr _configFunc)
        : staticNestConfig(_staticNestConfig), seqNum(_seqNum),
          configFunc(_configFunc) {}
      
    InstSeqNum getConfigSeqNum(uint64_t elementIdx) const;
    InstSeqNum getEndSeqNum(uint64_t elementIdx) const;
  };

  std::map<uint64_t, DynNestConfig *> activeDynNestConfigMap;

  struct StaticNestConfig {
    const ::LLVM::TDG::StreamRegion &region;
    ExecFuncPtr configFunc;
    std::list<DynNestConfig> dynConfigs;
    std::unordered_set<Stream *> baseStreams;
    std::unordered_set<Stream *> configStreams;
    StaticNestConfig(const ::LLVM::TDG::StreamRegion &_region,
                     ExecFuncPtr _configFunc)
        : region(_region), configFunc(_configFunc) {}
  };

  /**
   * Remember all static nest config.
   */
  std::unordered_map<std::string, StaticNestConfig> staticNestConfigMap;

  void configureNestStream(DynNestConfig &dynNestConfig);
};

#endif