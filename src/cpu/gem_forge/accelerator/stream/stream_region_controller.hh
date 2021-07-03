
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

  void tick();

  void takeOverBy(GemForgeCPUDelegator *newCPUDelegator);

  struct StaticRegion;
  struct DynRegion {
    StaticRegion *staticRegion;
    const uint64_t seqNum;
    bool configExecuted = false;

    DynRegion(StaticRegion *_staticRegion, uint64_t _seqNum)
        : staticRegion(_staticRegion), seqNum(_seqNum) {}

    /**
     * NestStream states.
     */
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
    std::vector<DynNestConfig> nestConfigs;

    /**
     * StreamLoopBound states.
     */
    struct DynLoopBound {
      ExecFuncPtr boundFunc = nullptr;
      DynamicStreamFormalParamV formalParams;
      uint64_t nextElementIdx = 0;
      // We have reached the end of the loop.
      bool brokenOut = false;
      // We have offloaded the LoopBound.
      bool offloaded = false;
    };
    DynLoopBound loopBound;

    /**
     * StreamStepper states for LoopEliminated region.
     */
    struct DynStep {
      uint64_t nextElementIdx = 0;
      int nextStepStreamIdx = 0;
      enum StepState {
        // There is no Execute stage for StreamStep.
        BEFORE_DISPATCH,
        BEFORE_COMMIT,
      };
      StepState state = StepState::BEFORE_DISPATCH;
    };
    DynStep step;
  };

  struct StaticRegion {
    using StreamSet = std::unordered_set<Stream *>;
    using StreamVec = std::vector<Stream *>;
    const ::LLVM::TDG::StreamRegion &region;
    StreamVec streams;
    std::list<DynRegion> dynRegions;
    StaticRegion(const ::LLVM::TDG::StreamRegion &_region) : region(_region) {}

    /**
     * NestStream config (for this nested region).
     */
    struct StaticNestConfig {
      ExecFuncPtr configFunc = nullptr;
      ExecFuncPtr predFunc = nullptr;
      bool predRet;
      StreamSet baseStreams;
    };
    StaticNestConfig nestConfig;

    /**
     * StreamLoopBound config.
     */
    struct StaticLoopBound {
      ExecFuncPtr boundFunc;
      bool boundRet;
      StreamSet baseStreams;
    };
    StaticLoopBound loopBound;

    /**
     * StreamStepper states for LoopEliminated region.
     */
    struct StaticStep {
      StreamVec stepRootStreams;
    };
    StaticStep step;
  };

  /******************************************************************
   * Util APIs.
   ******************************************************************/
  StaticRegion &getStaticRegion(const std::string &regionName);
  DynRegion &getDynRegion(const std::string &regionName, InstSeqNum seqNum);

  void receiveOffloadedLoopBoundRet(const DynamicStreamId &dynStreamId,
                                    int64_t tripCount, bool brokenOut);

private:
  StreamEngine *se;
  GemForgeISAHandler isaHandler;

  std::map<uint64_t, DynRegion *> activeDynRegionMap;

  /**
   * Remember all static region config.
   */
  std::unordered_map<std::string, StaticRegion> staticRegionMap;

  /**
   * For NestStream.
   */
  void initializeNestStreams(const ::LLVM::TDG::StreamRegion &region,
                             StaticRegion &staticRegion);
  void dispatchStreamConfigForNestStreams(const ConfigArgs &args,
                                          DynRegion &dynRegion);
  void executeStreamConfigForNestStreams(const ConfigArgs &args,
                                         DynRegion &dynRegion);
  void configureNestStream(DynRegion &dynRegion,
                           DynRegion::DynNestConfig &dynNestConfig);

  /**
   * For StreamLoopBound.
   */
  void initializeStreamLoopBound(const ::LLVM::TDG::StreamRegion &region,
                                 StaticRegion &staticRegion);
  void dispatchStreamConfigForLoopBound(const ConfigArgs &args,
                                        DynRegion &dynRegion);
  void executeStreamConfigForLoopBound(const ConfigArgs &args,
                                       DynRegion &dynRegion);
  void checkLoopBound(DynRegion &dynRegion);

  /**
   * For StreamStepper (LoopEliminatedRegion)
   */
  void initializeStep(const ::LLVM::TDG::StreamRegion &region,
                      StaticRegion &staticRegion);
  void dispatchStreamConfigForStep(const ConfigArgs &args,
                                   DynRegion &dynRegion);
  void stepStream(DynRegion &dynRegion);

  /**
   * Allocate stream elements.
   */
  void allocateElements(StaticRegion &staticRegion);

  /**
   * Helper functions.
   */
  DynRegion &pushDynRegion(StaticRegion &staticRegion, uint64_t seqNum);

  void buildFormalParams(const ConfigArgs::InputVec &inputVec, int &inputIdx,
                         const ::LLVM::TDG::ExecFuncInfo &funcInfo,
                         DynamicStreamFormalParamV &formalParams);

  struct GetStreamValueFromElementSet {
    using ElementSet = std::unordered_set<StreamElement *>;
    ElementSet &elements;
    const char *error;
    GetStreamValueFromElementSet(ElementSet &_elements, const char *_error)
        : elements(_elements), error(_error) {}
    StreamValue operator()(uint64_t streamId) const;
  };
};

#endif