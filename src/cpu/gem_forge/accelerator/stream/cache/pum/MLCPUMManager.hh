
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
    AffinePatternVecT splitOuterDims;
    std::string regionName;

    AffinePattern getPatternAdjustedByOuterIter(int64_t patternIdx,
                                                int64_t outerIter) const;
  };

  using StreamPatternInfoMapT = std::unordered_map<Stream *, PatternInfo>;

  struct PUMComputeStreamGroup {
    /**
     * Information gathered when compiling PUM.
     */
    ConfigPtr computeConfig;
    ConfigPtr reduceConfig;
    CacheStreamConfigureVec usedConfigs;
    bool outerDimSplitted = false;
    bool canApplyPUM = false;
    bool appliedPUM = false;

    /**
     * States during PUM execution.
     */

    // Track the next OuterIter when splitting OuterDim.
    int64_t nextOuterIter = 0;
  };

  /**
   * Track states of PUM.
   */
  struct PUMContext {
    /**
     * Information collected during analysis whether we can apply PUM or not.
     */
    // Records all the configs.
    CacheStreamConfigureVec configs;
    // DynamicStreamId that are now completely handled as PUM.
    std::vector<DynStreamId> purePUMStreamIds;
    // Records all the PUMComputeStreamGroups.
    std::vector<PUMComputeStreamGroup> pumGroups;
    // Records all the PatternInfo per stream.
    StreamPatternInfoMapT patternInfo;

    /**
     * States during compiling and executing PUM.
     */
    PUMCommandVecT commands;
    int configuredBanks = 0;
    int totalSentPackets = 0;
    int totalRecvPackets = 0;
    int totalAckBanks = 0;
    int totalSyncs = 0;
    int reachedSync = 0;
    void clear();

    enum StateE {
      Initialized, // Initialized.
      Kicked,      // LLC PUMEngine started.
      Done,        // LLC PUMEngine done.
    };
    StateE state = StateE::Initialized;
    bool isActive() const { return !configs.empty(); }

    /**
     * Stats for PUM computation.
     */
    Cycles initCycle = Cycles(0);     // When I was intialized.
    Cycles lastKickCycle = Cycles(0); // Last time I was kicked.
    Cycles lastSyncCycle = Cycles(0); // Last time I was synced.
  };
  using PUMContextListT = std::list<PUMContext>;
  PUMContextListT contexts;

  /**
   * Find all PUMComputeStreamGroups.
   */
  void findPUMComputeStreamGroups(PUMContext &context);

  /**
   * Clear PUMConfigs from normal configs handled by MLCStreamEngine.
   */
  void erasePUMConfigs(PUMContext &context, CacheStreamConfigureVec *configs,
                       const PUMComputeStreamGroup &group);

  /**
   * Check if PUM can be applied to a PUMComputeStreamGroup.
   */
  bool canApplyPUMToGroup(PUMContext &context, PUMComputeStreamGroup &group);

  /**
   * Compile the context once. It may require multiple compilation if we
   * splitted the outer dimension.
   */
  void compileContext(PUMContext &context);

  /**
   * Compute the Group to PUM.
   */
  void compileGroup(PUMContext &context, PUMComputeStreamGroup &group);

  /**
   * Compile data move for one forward edge.
   */
  void compileDataMove(PUMContext &context, PUMComputeStreamGroup &group,
                       const ConfigPtr &sendConfig);

  /**
   * Compile the computation instruction.
   */
  void compileCompute(PUMContext &context, PUMComputeStreamGroup &group);

  /**
   * Compile the final reduction instruction.
   */
  void compileReduction(PUMContext &context, PUMComputeStreamGroup &group,
                        PUMCommandVecT &commands);

  /**
   * Decoalesce and devectorize stream pattern.
   */
  AffinePatternVecT
  decoalesceAndDevectorizePattern(const ConfigPtr &config,
                                  const AffinePattern &pattern,
                                  int scalarElemSize);

  /**
   * Preprocess stream patterns to make them suitable for PUM.
   */
  void preprocessPatternsInGroup(PUMContext &context,
                                 PUMComputeStreamGroup &group);

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
  void kickPUMEngine(PUMContext &context, MessageSizeType sizeType,
                     bool isIdea);

  void checkSync(PUMContext &context);

  /**
   * Finish one round of computation. May start next round if needed.
   */
  void completeOneComputeRound(PUMContext &context);

  PUMContext &getFirstKickedContext();
  PUMContext *getFirstInitializedContext();

  void sendBackFinalReductionValue(const PUMContext &context,
                                   const PUMComputeStreamGroup &group);
};

#endif