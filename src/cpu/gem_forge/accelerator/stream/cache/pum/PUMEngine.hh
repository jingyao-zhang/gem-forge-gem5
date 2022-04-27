#ifndef __CPU_GEM_FORGE_PUM_ENGINE_HH__
#define __CPU_GEM_FORGE_PUM_ENGINE_HH__

#include "PUMCommand.hh"
#include "PUMHWConfiguration.hh"

#include "../LLCStreamEngine.hh"

/**
 * This represents the PUM engine at each LLC bank.
 * It is in charge of simulating the PUM commands and intereacts with
 * the LLC controller and NoC.
 */
class MLCPUMManager;
class PUMEngine {
public:
  PUMEngine(LLCStreamEngine *_se);

  void receiveKick(const RequestMsg &msg);
  void receiveData(const RequestMsg &msg);
  void configure(MLCPUMManager *pumManager, int64_t pumContextId,
                 const PUMCommandVecT &commands);
  void tick();

  bool hasCompletedRound(int64_t pumContextId, int rounds) const;
  bool hasStartedRound(int64_t pumContextId, int rounds) const;
  bool shouldWaitPUMRound(int64_t pumContextId, int rounds,
                          bool waitOnStart) const {
    if (waitOnStart) {
      return !this->hasStartedRound(pumContextId, rounds);
    } else {
      return !this->hasCompletedRound(pumContextId, rounds);
    }
  }

  void sendPUMDataToLLC(const DynStreamSliceId &sliceId,
                        const NetDest &recvBanks, int bytes);

  using SentPktMapT = std::map<NodeID, int>;
  void sendSyncToLLCs(const SentPktMapT &sentMap,
                      const DynStreamSliceId &sliceId);

  void sendSyncToMLC(int sentPackets);

private:
  LLCStreamEngine *se;
  AbstractStreamAwareController *controller;

  std::unique_ptr<PUMHWConfiguration> hwConfig;

  /**
   * States during each run.
   */
  MLCPUMManager *pumManager = nullptr;
  static constexpr int64_t InvalidPUMContextId =
      CacheStreamConfigureData::InvalidPUMContextId;
  int64_t pumContextId = InvalidPUMContextId;
  PUMCommandVecT commands;
  bool receivedConfig = false;
  int startedRound = -1;
  int completedRound = -1;
  int nextCmdIdx = 0;
  Cycles nextCmdReadyCycle;
  int sentPUMDataPkts = 0;
  int recvDataPkts = 0;
  SentPktMapT sentInterBankPacketMap;
  std::map<NodeID, std::pair<int, int>> recvPUMDataPktMap;
  std::map<DynStrandId, std::pair<int, int>> recvStreamDataPktMap;
  bool acked = false;

  int getBankIdx() const { return this->controller->getMachineID().num; }

  int curRemoteBank() const { return this->se->curRemoteBank(); }
  MachineType myMachineType() const { return this->se->myMachineType(); }
  const char *curRemoteMachineType() const {
    return this->se->curRemoteMachineType();
  }

  void kickNextCommand();
  Cycles estimateCommandLatency(const PUMCommand &command);

  void synced();

  void sendDoneToMLC(int recvPackets);
  void sendAckToMLC(CoherenceResponseType type, int ackCount);
  void sendSyncToLLC(MachineID recvBank, int sentPackets,
                     const DynStreamSliceId &sliceId);

  void receiveDataFromPUM(const RequestMsg &msg);
  void receiveDataFromStream(const RequestMsg &msg);
};

#endif