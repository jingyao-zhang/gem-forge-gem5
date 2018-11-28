#ifndef __CPU_TDG_ACCELERATOR_HH__
#define __CPU_TDG_ACCELERATOR_HH__

#include "base/types.hh"
#include "params/TDGAcceleratorManager.hh"
#include "sim/sim_object.hh"

#include <list>

class LLVMDynamicInst;
class LLVMTraceCPU;
class TDGAcceleratorManager;

class TDGAccelerator {
public:
  TDGAccelerator() {}
  virtual ~TDGAccelerator() {}

  TDGAccelerator(const TDGAccelerator &other) = delete;
  TDGAccelerator(TDGAccelerator &&other) = delete;
  TDGAccelerator &operator=(const TDGAccelerator &other) = delete;
  TDGAccelerator &operator=(TDGAccelerator &&other) = delete;

  virtual void handshake(LLVMTraceCPU *_cpu, TDGAcceleratorManager *_manager);

  virtual bool handle(LLVMDynamicInst *inst) = 0;
  virtual void tick() = 0;

  virtual void dump() {}

  LLVMTraceCPU *getCPU() { return this->cpu; }

  /**
   * Called by the manager to register stats.
   * Default does nothing.
   */
  virtual void regStats() {}

protected:
  LLVMTraceCPU *cpu;
  TDGAcceleratorManager *manager;
};

class StreamEngine;
class TDGAcceleratorManager : public SimObject {
public:
  TDGAcceleratorManager(TDGAcceleratorManagerParams *params);
  ~TDGAcceleratorManager();

  TDGAcceleratorManager(const TDGAcceleratorManager &other) = delete;
  TDGAcceleratorManager(TDGAcceleratorManager &&other) = delete;
  TDGAcceleratorManager &operator=(const TDGAcceleratorManager &other) = delete;
  TDGAcceleratorManager &operator=(TDGAcceleratorManager &&other) = delete;

  void addAccelerator(TDGAccelerator *accelerator);

  void handshake(LLVMTraceCPU *_cpu);
  void handle(LLVMDynamicInst *inst);
  void tick();
  void dump();

  void exitDump();

  StreamEngine *getStreamEngine();

  // void useStream(uint64_t streamId, const LLVMDynamicInst *user);
  // bool isStreamReady(uint64_t streamId, const LLVMDynamicInst *user) const;
  // bool canStreamStep(uint64_t streamId) const;
  // void commitStreamConfigure(uint64_t streamId, uint64_t configSeqNum);
  // void commitStreamStep(uint64_t streamId, uint64_t stepSeqNum);
  // void commitStreamStore(uint64_t streamId, uint64_t storeSeqNum);
  // void commitStreamEnd(uint64_t streamId, uint64_t storeSeqNum);

  void regStats() override;

private:
  std::list<TDGAccelerator *> accelerators;
};

#endif
