
#ifndef __CPU_GEM_FORGE_ACCELERATOR_NEST_STREAM_CONTROLLER_H__
#define __CPU_GEM_FORGE_ACCELERATOR_NEST_STREAM_CONTROLLER_H__

#include "stream_engine.hh"

class StreamRegionController {
public:
  StreamRegionController(StreamEngine *_se);
  ~StreamRegionController();

  void initializeRegion(const ::LLVM::TDG::StreamRegion &region);

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

  struct StaticRegion;
  struct DynRegion {
    const StaticRegion *staticRegion;
    const uint64_t seqNum;
    bool configExecuted = false;

    struct DynNestConfig {
      const StaticRegion *staticRegion;
      ExecFuncPtr configFunc = nullptr;
      ExecFuncPtr predFunc = nullptr;
      DynamicStreamFormalParamV formalParams;
      DynamicStreamFormalParamV predFormalParams;
      uint64_t nextElementIdx = 0;

      DynNestConfig(const StaticRegion *_staticRegion)
          : staticRegion(_staticRegion) {}

      InstSeqNum getConfigSeqNum(uint64_t elementIdx, uint64_t outSeqNum) const;
      InstSeqNum getEndSeqNum(uint64_t elementIdx, uint64_t outSeqNum) const;
    };
    /**
     * ! NestConfig for nested regions.
     * ! Must use list 
     */
    std::vector<DynNestConfig> nestConfigs;

    DynRegion(StaticRegion *_staticRegion, uint64_t _seqNum)
        : staticRegion(_staticRegion), seqNum(_seqNum) {}
  };

  std::map<uint64_t, DynRegion *> activeDynRegionMap;

  struct StaticRegion {
    const ::LLVM::TDG::StreamRegion &region;
    struct StaticNestConfig {
      ExecFuncPtr configFunc = nullptr;
      ExecFuncPtr predFunc = nullptr;
      bool predRet;
      std::unordered_set<Stream *> baseStreams;
      std::unordered_set<Stream *> configStreams;
    };
    // NestConfig for this Region.
    StaticNestConfig nestConfig;
    std::list<DynRegion> dynRegions;
    StaticRegion(const ::LLVM::TDG::StreamRegion &_region) : region(_region) {}
  };

  /**
   * Remember all static nest config.
   */
  std::unordered_map<std::string, StaticRegion> staticRegionMap;

  void initializeNestStreams(const ::LLVM::TDG::StreamRegion &region,
                             StaticRegion &staticRegion);
  void dispatchStreamConfigForNestStreams(const ConfigArgs &args,
                                          DynRegion &dynRegion);
  void executeStreamConfigForNestStreams(const ConfigArgs &args,
                                         DynRegion &dynRegion);
  void configureNestStream(DynRegion &dynRegion,
                           DynRegion::DynNestConfig &dynNestConfig);

  /**
   * Helper functions.
   */
  StaticRegion &getStaticRegion(const std::string &regionName);
  DynRegion &pushDynRegion(StaticRegion &staticRegion, uint64_t seqNum);
};

#endif