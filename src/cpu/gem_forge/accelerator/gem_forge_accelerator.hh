#ifndef __CPU_GEM_FORGE_ACCELERATOR_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_HH__

#include "cpu/gem_forge/gem_forge_cpu_delegator.hh"

#include "base/types.hh"
#include "sim/sim_object.hh"

#include "params/GemForgeAccelerator.hh"
#include "params/GemForgeAcceleratorManager.hh"

#include <vector>

class LLVMDynamicInst;
class LLVMTraceCPU;
class GemForgeAcceleratorManager;

class GemForgeAccelerator : public SimObject {
public:
  GemForgeAccelerator(GemForgeAcceleratorParams *params)
      : SimObject(params), cpuDelegator(nullptr), manager(nullptr) {}
  virtual ~GemForgeAccelerator() {}

  GemForgeAccelerator(const GemForgeAccelerator &other) = delete;
  GemForgeAccelerator(GemForgeAccelerator &&other) = delete;
  GemForgeAccelerator &operator=(const GemForgeAccelerator &other) = delete;
  GemForgeAccelerator &operator=(GemForgeAccelerator &&other) = delete;

  virtual void handshake(GemForgeCPUDelegator *_cpuDelegator,
                         GemForgeAcceleratorManager *_manager);
  virtual void takeOverBy(GemForgeCPUDelegator *newCpuDelegator,
                          GemForgeAcceleratorManager *newManager);

  /**
   * TODO: Finally decouple accelerator model from instruction.
   */
  virtual bool handle(LLVMDynamicInst *inst) { return false; }
  virtual void tick() = 0;

  virtual void dump() {}
  virtual bool checkProgress() { return false; }

  /**
   * Called by the manager to register stats.
   * Default does nothing.
   */
  void regStats() override { SimObject::regStats(); }
  /**
   * Will be invoked if you have self-defined stats (not from stats namespace).
   */
  virtual void resetStats() {}

  GemForgeCPUDelegator *getCPUDelegator() { return this->cpuDelegator; }

  Cycles curCycle() const { return this->cpuDelegator->curCycle(); }

protected:
  GemForgeCPUDelegator *cpuDelegator;
  GemForgeAcceleratorManager *manager;
};

class StreamEngine;
class SpeculativePrecomputationManager;
class GemForgeAcceleratorManager : public SimObject {
public:
  GemForgeAcceleratorManager(GemForgeAcceleratorManagerParams *params);
  ~GemForgeAcceleratorManager();

  GemForgeAcceleratorManager(const GemForgeAcceleratorManager &other) = delete;
  GemForgeAcceleratorManager(GemForgeAcceleratorManager &&other) = delete;
  GemForgeAcceleratorManager &
  operator=(const GemForgeAcceleratorManager &other) = delete;
  GemForgeAcceleratorManager &
  operator=(GemForgeAcceleratorManager &&other) = delete;

  void addAccelerator(GemForgeAccelerator *accelerator);

  void handshake(GemForgeCPUDelegator *_cpuDelegator);
  void handle(LLVMDynamicInst *inst);
  void tick();
  // Take all accelerators from old one.
  void takeOverFrom(GemForgeAcceleratorManager *oldManager);
  // Allow the accelerator to schedule tick() in next event.
  void scheduleTickNextCycle();
  void dump();
  bool checkProgress();
  void resetStats();

  void exitDump();

  StreamEngine *getStreamEngine();
  SpeculativePrecomputationManager *getSpeculativePrecomputationManager();

  void regStats() override;

private:
  std::vector<GemForgeAccelerator *> &accelerators;
  GemForgeCPUDelegator *cpuDelegator;

  EventFunctionWrapper tickEvent;
};

#endif
