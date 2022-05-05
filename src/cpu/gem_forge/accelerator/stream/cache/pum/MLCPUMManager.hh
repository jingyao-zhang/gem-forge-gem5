
#ifndef __CPU_GEM_FORGE_MLC_PUM_MANAGER_HH__
#define __CPU_GEM_FORGE_MLC_PUM_MANAGER_HH__

#include "../MLCStreamEngine.hh"

#include "DataMoveCompiler.hh"
#include <unordered_set>

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
   * Magic: Used to notify prefetch stream has completed.
   */
  void notifyPrefetchStreamComplete();

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

  /**
   * Number of in-flight prefetch streams.
   * Assumption: PUM currently has no time slicing capabilities.
   */
  int inFlightPrefetchStreams = 0;
  PacketPtr savedPkt = nullptr;

  struct PatternInfo {
    AffinePattern pattern;
    AffinePattern pumTile;
    Addr regionVAddr = 0;
    int scalarElemSize = 0;
    AffinePatternVecT atomicPatterns;
    AffinePatternVecT splitOuterDims;
    std::string regionName;

    AffinePattern getPatternAdjustedByOuterIter(int64_t patternIdx,
                                                int64_t outerIter) const;
    AffinePattern getPattern(int64_t patIdx) const {
      assert(patIdx < this->atomicPatterns.size());
      return this->atomicPatterns.at(patIdx);
    }
    AffinePattern getSplitOutDim(int64_t patIdx) const {
      assert(patIdx < this->atomicPatterns.size());
      if (patIdx < this->splitOuterDims.size()) {
        return this->splitOuterDims.at(patIdx);
      } else {
        // Return a default empty pattern.
        return AffinePattern();
      }
    }
    const AffinePattern &getSingleAtomicPat() const {
      assert(this->atomicPatterns.size() == 1);
      return this->atomicPatterns.front();
    }
    int64_t getLoadComputeResultPatternIdx() const {
      /**
       * As a heuristic, LoadComputeResult uses the middle Pattern as the result
       * pattern.
       */
      return this->atomicPatterns.size() / 2;
    }
  };

  using StreamPatternInfoMapT = std::unordered_map<Stream *, PatternInfo>;

  struct PUMComputeStreamGroup {
    /**
     * Information gathered when compiling PUM.
     */
    ConfigPtr computeConfig;
    ConfigPtr reduceConfig;
    CacheStreamConfigureVec usedConfigs;
    CacheStreamConfigureVec usedPUMConfigs;
    CacheStreamConfigureVec usedNonPUMConfigs;
    bool outerDimSplitted = false;
    bool canApplyPUM = false;
    bool appliedPUM = false;

    // Records all the PatternInfo per stream.
    StreamPatternInfoMapT patternInfo;

    /**
     * Information about offloaded PUMReducetion stream.
     */
    ConfigPtr pumDirectConfig;
    ConfigPtr pumReduceConfig;

    /**
     * States during PUM execution.
     */

    // Track the next OuterIter when splitting OuterDim.
    int64_t nextOuterIter = 0;

    /**
     * States for reduction results.
     */
    struct ReductionResult {
      uint64_t elemIdx;
      StreamValue value;
      ReductionResult(uint64_t _elemIdx) : elemIdx(_elemIdx) { value.fill(0); }
    };
    std::list<ReductionResult> reductionResults;
  };

  /**
   * Represent the PUM DataGraph.
   */
  struct PUMDataGraphNode {

    enum TypeE {
      // A value that already in PUM transposed format.
      Value,
      // A move node, may have reuse.
      Move,
      // A load node, uses normal streams to poplulate PUM region.
      Load,
      // A compute node execute the function.
      Compute,
      // A sync node represents the global barrier.
      Sync,
    };
    // Common fields.
    std::string regionName;
    const TypeE type;
    const AffinePattern pumTile;
    AffinePattern pattern;
    AffinePattern splitOutDim; // Split outer dimension.
    int scalarElemSize;
    int startWordline = 0;
    std::vector<PUMDataGraphNode *> operands;
    std::vector<PUMDataGraphNode *> users;

    void replaceUsedBy(PUMDataGraphNode *newNode) {
      for (auto user : this->users) {
        bool replaced = false;
        for (auto &operand : user->operands) {
          if (operand == this) {
            replaced = true;
            operand = newNode;
          }
        }
        assert(replaced);
        newNode->users.push_back(user);
      }
    }

    AffinePattern adjustPatByOutIter(const AffinePattern &pat,
                                     const AffinePattern &splitOutDim,
                                     int64_t outIter) const {
      if (splitOutDim.getTotalTrip() == 0) {
        // There is no SplitOutDim.
        return pat;
      } else {
        auto outOffset = splitOutDim(outIter);
        auto ret = pat;
        ret.start += outOffset;
        return ret;
      }
    }

    AffinePattern adjustPatByOutIter(int64_t outIter) const {
      return this->adjustPatByOutIter(this->pattern, this->splitOutDim,
                                      outIter);
    }

    /**
     * Fields for Value node.
     */
    Addr regionVAddr = 0;
    static PUMDataGraphNode *newValueNode(const std::string &_regionName,
                                          const AffinePattern &_pumTile,
                                          const AffinePattern &_pattern,
                                          const AffinePattern &_splitOutDim,
                                          int _scalarElemSize,
                                          Addr _regionVAddr);

    /**
     * Fields for Move node.
     */
    AffinePattern sendPat;
    AffinePattern sendSplitOutDim;
    static PUMDataGraphNode *newMoveNode(
        const std::string &_regionName, const AffinePattern &_pumTile,
        const AffinePattern &_pattern, const AffinePattern &_splitOutDim,
        const AffinePattern &_sendPat, const AffinePattern &_sendSplitOutDim,
        PUMDataGraphNode *_sendNode, int _scalarElemSize);
    AffinePattern adjustSendPatByOutIter(int64_t outIter) const {
      return this->adjustPatByOutIter(this->sendPat, this->sendSplitOutDim,
                                      outIter);
    }

    /**
     * Fields for Load node.
     * Represents a LoadStream collecting data for PUM computation.
     * It shares the field of sendPat and sendSplitOutDim.
     */
    ConfigPtr sendConfig;
    ConfigPtr recvConfig;
    static PUMDataGraphNode *
    newLoadNode(const std::string &_regionName, AffinePattern &_pumTile,
                const AffinePattern &_pattern,
                const AffinePattern &_splitOutDim,
                const AffinePattern &_sendPat, ConfigPtr _sendConfig,
                ConfigPtr _recvConfig, int _scalarElemSize);

    /**
     * Fields for Compute node.
     */
    ExecFuncPtr func = nullptr;
    PUMComputeStreamGroup *group = nullptr;
    static PUMDataGraphNode *newCmpNode(const std::string &_regionName,
                                        const AffinePattern &_pumTile,
                                        const AffinePattern &_pattern,
                                        const AffinePattern &_splitOutDim,
                                        int _scalarElemSize, ExecFuncPtr _func,
                                        PUMComputeStreamGroup *_group);

    /**
     * Fields for Sync node.
     */
    static PUMDataGraphNode *newSyncNode();

  private:
    // A basic constructor for basic fields.
    PUMDataGraphNode(const std::string &_regionName, TypeE _type,
                     const AffinePattern &_pumTile,
                     const AffinePattern &_pattern,
                     const AffinePattern &_splitOutDim, int _scalarElemSize)
        : regionName(_regionName), type(_type), pumTile(_pumTile),
          pattern(_pattern), splitOutDim(_splitOutDim),
          scalarElemSize(_scalarElemSize) {}
  };

  friend std::ostream &operator<<(std::ostream &os,
                                  const MLCPUMManager::PUMDataGraphNode &node);
  friend std::string to_string(const MLCPUMManager::PUMDataGraphNode &node);

  /**
   * Track states of PUM.
   */
  static constexpr int64_t InvalidPUMContextId =
      CacheStreamConfigureData::InvalidPUMContextId;

  struct PUMContext {

  private:
    static int64_t nextContextId;
    static int64_t allocateContextId() { return nextContextId++; }

  public:
    ~PUMContext();

    /**
     * Information collected during analysis whether we can apply PUM or not.
     */
    // Unique context Id.
    const int64_t contextId = allocateContextId();
    // Records all the configs.
    CacheStreamConfigureVec configs;
    // DynamicStreamId that are now completely handled as PUM.
    std::vector<DynStreamId> purePUMStreamIds;
    // Records all the PUMComputeStreamGroups.
    std::vector<PUMComputeStreamGroup> pumGroups;
    // Records the PUMDataGraph.
    std::vector<PUMDataGraphNode *> pumDataGraphNodes;

    /**
     * States during compiling and executing PUM.
     */
    // Next Out Iter.
    int64_t nextOutIter = 0;
    PUMCommandVecT commands;
    std::vector<int> expectedAcksEverySync;
    int totalSentPackets = 0;
    int totalRecvPackets = 0;
    int receivedAcks = 0;
    int totalSyncs = 0;
    int reachedSync = 0;
    void clear();
    void clearPUMDataGraphNodes();

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
    bool waitingPostConfig = true;    // Expecting post configuration.
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
   * Add the special reduction stream if the PUMComputeStreamGroup contains
   * reduction. This is a normal offloaded stream with special step pattern that
   * skips already reduced elements.
   */
  void addPUMReduceStream(PUMContext &context, CacheStreamConfigureVec *configs,
                          PUMComputeStreamGroup &group);

  /**
   * Add the special LoadStream if the PUMComputeStreamGroup uses NonPUMConfigs.
   * This is a normal offloaded stream with special broadcast support to load
   * data in non-PUM region to PUM transposed format.
   */
  void addPUMLoadStream(PUMContext &context, CacheStreamConfigureVec *configs,
                        PUMDataGraphNode *loadNode);

  /**
   * Check if PUM can be applied to a PUMComputeStreamGroup.
   */
  bool canApplyPUMToGroup(PUMContext &context, PUMComputeStreamGroup &group);

  /**
   * Build the PUMDataGraph.
   */
  using PUMDataGraphNodeVec = std::vector<PUMDataGraphNode *>;
  void buildPUMDataGraph(PUMContext &context);
  void buildPUMDataGraph(PUMContext &context, PUMComputeStreamGroup &group);
  void buildPUMDataGraphMove(PUMContext &context, PUMComputeStreamGroup &group,
                             const ConfigPtr &sendConfig,
                             PUMDataGraphNodeVec &resultNodes);
  void buildPUMDataGraphLoad(PUMContext &context, PUMComputeStreamGroup &group,
                             const ConfigPtr &sendConfig,
                             PUMDataGraphNodeVec &resultNodes);
  void buildPUMDataGraphCompute(PUMContext &context,
                                PUMComputeStreamGroup &group,
                                const PUMDataGraphNodeVec &moveNodes);
  bool needExpandReuse(PUMContext &context, const PUMComputeStreamGroup &group);
  AffinePattern expandReusePat(const AffinePattern &pumTile,
                               const AffinePattern &pat,
                               AffinePattern &splitOutDim);

  /**
   * Try to merge some Move nodes if they are moving the same array but only
   * small boundary difference.
   */
  void mergePUMDataGraphMoveNode(PUMContext &context);

  /**
   * Schedule PUMDataGraph nodes and insert sync nodes.
   * So far just BFS.
   */
  PUMDataGraphNodeVec schedulePUMDataGraph(PUMContext &context);

  /**
   * Compile scheduled PUMDataGraph into commands.
   */
  void compilePUMDataGraphToCommands(PUMContext &context);

  /**
   * Compile data move for one node.
   */
  void compileDataMove(PUMContext &context, PUMDataGraphNode *node);

  /**
   * Compile one sync node.
   */
  void compileSync(PUMContext &context, PUMDataGraphNode *node);

  /**
   * Compile the computation instruction.
   */
  void compileCompute(PUMContext &context, PUMDataGraphNode *node);

  /**
   * Compile the final reduction instruction.
   */
  void compileReduction(PUMContext &context, PUMComputeStreamGroup &group,
                        PUMCommandVecT &commands);

  /**
   * Compile the context once. It may require multiple compilation if we
   * splitted the outer dimension.
   */
  void compileContext(PUMContext &context);

  /**
   * Run prefetch stage.
   */
  void runPrefetchStage(CacheStreamConfigureVec *configs);

  /**
   * Run PUM execution stage.
   */
  void runPUMExecutionStage();

  /**
   * Dispatch stream configurations to MLCSE.
   */
  void dispatchStreamConfigs(CacheStreamConfigureVec *configs) const;

  /**
   * Some work needs to be done after MLC SE finish configuring.
   * For example, to know how many strands of each LoadStream so that we
   * can correctly set the number of expected Acks.
   */
  void postMLCSEConfigure();

  /**
   * Conditionally prefetches data from DRAM->LLC.
   */
  CacheStreamConfigureVec generatePrefetchStreams(PUMComputeStreamGroup &group);
  /**
   * Decoalesce and devectorize stream pattern.
   */
  AffinePatternVecT
  decoalesceAndDevectorizePattern(const ConfigPtr &config,
                                  const AffinePattern &pattern,
                                  int scalarElemSize);

  /**
   * @brief Convert an AffinePattern back to LinearAddrGen params.
   * This needs the starting VAddr of the array and the data type size.
   */
  DynStreamFormalParamV convertAffinePatternToStreamFormalParams(
      const AffinePattern &pattern, Addr arrayVAddr, int64_t memElemSize) const;

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

  /**
   * @brief Try to start another round of computation. Check that all consuming
   * streams have caught up. So far this is only used for PUMReduction.
   */
  void tryKickNextComputeRound(PUMContext &context);

  struct TryKickContextCallback {
    MLCPUMManager *manager;
    int64_t contextId;
    TryKickContextCallback(MLCPUMManager *_manager, int64_t _contextId)
        : manager(_manager), contextId(_contextId) {}
    void operator()(const DynStreamId &dynId, uint64_t elemIdx) {
      manager->tryKickNextComputeRound(manager->getContextById(contextId));
    }
  };

  PUMContext &getFirstKickedContext();
  PUMContext *getFirstInitializedContext();
  PUMContext &getContextById(int64_t contextId);

  /**
   * @brief Complete one round of reduction.
   *
   * @param context PUMContext.
   * @param group PUMComputeStreamGroup with the reduction
   */
  void completeFinalReduction(PUMContext &context,
                              PUMComputeStreamGroup &group);

  void sendOneReductionResult(PUMContext &context,
                              PUMComputeStreamGroup &group);
};

std::ostream &operator<<(std::ostream &os,
                         const MLCPUMManager::PUMDataGraphNode &node);

std::string to_string(const MLCPUMManager::PUMDataGraphNode &node);

#endif
