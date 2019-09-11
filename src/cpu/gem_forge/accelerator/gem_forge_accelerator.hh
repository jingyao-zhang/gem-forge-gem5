#ifndef __CPU_GEM_FORGE_ACCELERATOR_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_HH__

#include "base/types.hh"
#include "params/GemForgeAccelerator.hh"
#include "params/GemForgeAcceleratorManager.hh"
#include "sim/sim_object.hh"

#include <vector>

class LLVMDynamicInst;
class LLVMTraceCPU;
class GemForgeAcceleratorManager;

class GemForgeAccelerator : public SimObject {
public:
  GemForgeAccelerator(GemForgeAcceleratorParams *params) : SimObject(params) {}
  virtual ~GemForgeAccelerator() {}

  GemForgeAccelerator(const GemForgeAccelerator &other) = delete;
  GemForgeAccelerator(GemForgeAccelerator &&other) = delete;
  GemForgeAccelerator &operator=(const GemForgeAccelerator &other) = delete;
  GemForgeAccelerator &operator=(GemForgeAccelerator &&other) = delete;

  virtual void handshake(LLVMTraceCPU *_cpu,
                         GemForgeAcceleratorManager *_manager);

  virtual bool handle(LLVMDynamicInst *inst) = 0;
  virtual void tick() = 0;

  virtual void dump() {}

  LLVMTraceCPU *getCPU() { return this->cpu; }

  /**
   * Called by the manager to register stats.
   * Default does nothing.
   */
  void regStats() override { SimObject::regStats(); }

protected:
  LLVMTraceCPU *cpu;
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

  void handshake(LLVMTraceCPU *_cpu);
  void handle(LLVMDynamicInst *inst);
  void tick();
  void dump();

  void exitDump();

  StreamEngine *getStreamEngine();
  SpeculativePrecomputationManager *getSpeculativePrecomputationManager();

  void regStats() override;

private:
  std::vector<GemForgeAccelerator *> &accelerators;
};

#endif
