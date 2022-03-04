
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
   * @return true if applied PUM.
   */
  bool receiveStreamConfigure(PacketPtr pkt);

  /**
   * Receive a StreamEnd message and release the PUM context.
   * @return true if this is handled as PUM.
   */
  bool receiveStreamEnd(PacketPtr pkt);

  /**
   * APIs for PUMEngine.
   */
  void reachSync(int sentPackets);
  void receivePacket();

private:
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
  struct Args {
    CacheStreamConfigureVec *configs;
    StreamPatternInfoMapT patternInfo;
    int numStoreStreams = 0;
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
    CacheStreamConfigureVec *configs = nullptr;
    PUMCommandVecT commands;
    bool done = false;
    void clear();
    bool isActive() const { return configs != nullptr; }
  };
  PUMContext context;

  /**
   * Convert the LinearAddressPattern to AffinePattern.
   * Notice that it takes care of the element size.
   * @return EmptyPattern if we failed to convert.
   */
  bool canApplyPUM(Args &args, const CacheStreamConfigureDataPtr &config);

  /**
   * Check if we can apply PUM.
   */
  bool canApplyPUM(Args &args);

  /**
   * Decoalesce stream pattern.
   */
  AffinePatternVecT
  decoalesceAndDevectorizePattern(const CacheStreamConfigureDataPtr &config,
                                  const AffinePattern &pattern,
                                  int scalarElemSize);

  /**
   * Generate the commands.
   */
  void applyPUM(Args &args);
  void compileDataMove(Args &args, const CacheStreamConfigureDataPtr &config);

  void configurePUMEngine(Args &args);

  void checkDone();
};

#endif