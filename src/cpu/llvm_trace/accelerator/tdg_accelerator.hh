#ifndef __CPU_TDG_ACCELERATOR_HH__
#define __CPU_TDG_ACCELERATOR_HH__

#include "base/types.hh"

#include <list>

class LLVMDynamicInst;
class LLVMTraceCPU;

class TDGAccelerator {
public:
  TDGAccelerator() {}
  virtual ~TDGAccelerator() {}

  TDGAccelerator(const TDGAccelerator &other) = delete;
  TDGAccelerator(TDGAccelerator &&other) = delete;
  TDGAccelerator &operator=(const TDGAccelerator &other) = delete;
  TDGAccelerator &operator=(TDGAccelerator &&other) = delete;

  void handshake(LLVMTraceCPU *_cpu);

  virtual bool handle(LLVMDynamicInst *inst) = 0;
  virtual void tick() = 0;

protected:
  LLVMTraceCPU *cpu;
};

class TDGAcceleratorManager {
public:
  TDGAcceleratorManager();
  ~TDGAcceleratorManager();

  TDGAcceleratorManager(const TDGAcceleratorManager &other) = delete;
  TDGAcceleratorManager(TDGAcceleratorManager &&other) = delete;
  TDGAcceleratorManager &operator=(const TDGAcceleratorManager &other) = delete;
  TDGAcceleratorManager &operator=(TDGAcceleratorManager &&other) = delete;

  void addAccelerator(TDGAccelerator *accelerator);

  void handshake(LLVMTraceCPU *_cpu);
  void handle(LLVMDynamicInst *inst);
  void tick();

private:
  std::list<TDGAccelerator *> accelerators;
};

#endif