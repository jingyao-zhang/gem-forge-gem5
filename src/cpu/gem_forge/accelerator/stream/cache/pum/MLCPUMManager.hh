
#ifndef __CPU_GEM_FORGE_MLC_PUM_MANAGER_HH__
#define __CPU_GEM_FORGE_MLC_PUM_MANAGER_HH__

#include "../MLCStreamEngine.hh"

#include "DataMoveCompiler.hh"

class MLCPUMManager {
public:
  MLCPUMManager(MLCStreamEngine *_mlcSE);
  ~MLCPUMManager();

  /**
   * Receive a StreamConfig message and generate PUM commands.
   * @return may modify the Configs in pkt to erase those handled as PUM.
   *
   * 1. First we find candidate PUMComputeStreams. Basically,
   * they are AffineStreams with StoreFunction, or ReduceStreams.
   * We call each PUMComputeStream and all its UsedByStreams
   * forms a PUMComputeStreamGroup.
   *
   * 2. For each PUMComputeStreamGroup, we check if we can offload it:
   *   a. All streams involved are AffineStream.
   *   b. Their mapping is aligned in StreamNUCAMap.
   *   c. Loop is eliminated.
   *   d. Known trip count.
   *   e. For stream patterns:
   *      StoreComputeStream must be a sub-region.
   *      LoadForwardStream must be able to reduce to a sub-region,
   *      with matched dimension with the StoreComputeStream.
   *   f. TODO: Enough wordlines to hold inputs and intermediate data.
   *
   * 3. For each of offloadable PUMComputeStreamGroup, we generate
   * data move and compute commands. Then we remove the DepEdges
   * from the UsedSBytreams.
   *
   * 4. Finally, Offloaded PUMComputeStream and UsedByStreams without
   * unoffloaded DepEdges can be considered handled by PUM now. Otherwise,
   * they are still offloaded as normal streams.
   * NOTE: One stream can be offloaded as PUM and normal stream at the
   * same time. The most common case is a LoadStream is used by two
   * StoreComputeStreams, one as PUM and one as normal stream. Then the
   * LoadStream need to be splitted ><.
   */
  void receiveStreamConfigure(PacketPtr pkt);

  /**
   * Receive a StreamEnd message and release the PUM context.
   */
  void receiveStreamEnd(PacketPtr pkt);

  /**
   * APIs for PUMEngine.
   */
  void reachSync(int sentPackets);
  void receivePacket(int recvPackets);

  MachineID getMachineID() const { return this->controller->getMachineID(); }

private:
  using ConfigPtr = CacheStreamConfigureDataPtr;
  MLCStreamEngine *mlcSE;
  AbstractStreamAwareController *controller;

  struct PatternInfo {
    AffinePattern pattern;
    AffinePattern pumTile;
    int scalarElemSize = 0;
    AffinePatternVecT atomicPatterns;
    std::string regionName;
  };

  using StreamPatternInfoMapT = std::unordered_map<Stream *, PatternInfo>;

  struct PUMComputeStreamGroup {
    ConfigPtr computeConfig;
    CacheStreamConfigureVec usedConfigs;
    bool hasReduction = false;
    bool canApplyPUM = false;
    bool appliedPUM = false;
  };

  /**
   * States during compiling PUM.
   */
  struct CompileStates {
    // Records all the configs.
    CacheStreamConfigureVec configs;
    // Records all the PUMComputeStreamGroups.
    std::vector<PUMComputeStreamGroup> pumGroups;
    // Records all the compiled data move.
    std::set<std::pair<Stream *, Stream *>> compiledDataMove;
    // Records all the PatternInfo per stream.
    StreamPatternInfoMapT patternInfo;
    PUMCommandVecT commands;
  };

  /**
   * States during the PUM.
   */
  struct PUMContext {
    int configuredBanks = 0;
    int totalSentPackets = 0;
    int totalRecvPackets = 0;
    int totalAckBanks = 0;
    CacheStreamConfigureVec configs;
    // DynamicStreamId that are now completely handled as PUM.
    std::vector<DynStreamId> purePUMStreamIds;
    std::vector<PUMComputeStreamGroup> pumGroups;
    PUMCommandVecT commands;
    int numSync = 0;
    void clear();
    enum StateE {
      Initialized, // Initialized.
      Kicked,      // LLC PUMEngine started.
      Done,        // LLC PUMEngine done.
    };
    StateE state = StateE::Initialized;
    bool isActive() const { return !configs.empty(); }
  };
  using PUMContextListT = std::list<PUMContext>;
  PUMContextListT contexts;

  /**
   * Find all PUMComputeStreamGroups.
   */
  void findPUMComputeStreamGroups(CompileStates &states);

  /**
   * Check if PUM can be applied to a PUMComputeStreamGroup.
   */
  bool canApplyPUMToGroup(CompileStates &states,
                          const PUMComputeStreamGroup &group);

  /**
   * Apply PUM to the Group.
   */
  void applyPUMToGroup(CompileStates &states, PUMComputeStreamGroup &group);

  /**
   * Compile data move for one forward edge.
   */
  void compileDataMove(CompileStates &states, const ConfigPtr &sendConfig,
                       const CacheStreamConfigureData::DepEdge &dep);

  void compileCompute(CompileStates &states, PUMComputeStreamGroup &group);

  /**
   * Decoalesce and devectorize stream pattern.
   */
  AffinePatternVecT
  decoalesceAndDevectorizePattern(const ConfigPtr &config,
                                  const AffinePattern &pattern,
                                  int scalarElemSize);

  /**
   * Translate outer-loop stream into inner-loop stream pattern, with the reuse
   * explicitly represented in the pattern.
   */
  AffinePattern addReuseToOuterPattern(const ConfigPtr &outerConfig,
                                       const ConfigPtr &innerConfig,
                                       const AffinePattern &pattern) const;

  void configurePUMEngine(PUMContext &context);

  /**
   * Send out an kick message to PUMEngine to continue execution.
   */
  void kickPUMEngine(MessageSizeType sizeType, bool isIdea);

  void checkSync(PUMContext &context);

  PUMContext &getFirstKickedContext();
  PUMContext *getFirstInitializedContext();

  void sendBackFinalReductionValue(const CacheStreamConfigureDataPtr &config);
};

#endif