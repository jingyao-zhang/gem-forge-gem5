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

  void receiveConfigure(const RequestMsg &msg);
  void receiveData(const RequestMsg &msg);
  void configure(MLCPUMManager *pumManager, const PUMCommandVecT &commands);

  void tick();

private:
  LLCStreamEngine *se;
  AbstractStreamAwareController *controller;

  std::unique_ptr<PUMHWConfiguration> hwConfig;

  /**
   * States during each run.
   */
  MLCPUMManager *pumManager = nullptr;
  PUMCommandVecT commands;
  int nextCmdIdx = 0;
  Cycles nextCmdReadyCycle;
  int sentInterBankPackets = 0;
  bool acked = false;

  int getBankIdx() const { return this->controller->getMachineID().num; }

  int curRemoteBank() const { return this->se->curRemoteBank(); }
  MachineType myMachineType() const { return this->se->myMachineType(); }
  const char *curRemoteMachineType() const {
    return this->se->curRemoteMachineType();
  }

  void kickNextCommand();
  Cycles estimateCommandLatency(const PUMCommand &command);
};

#endif