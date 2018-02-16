#ifndef __LLVM_TRACE_CPU_DRIVER_HH__
#define __LLVM_TRACE_CPU_DRIVER_HH__

#include "params/LLVMTraceCPUDriver.hh"
#include "sim/emul_driver.hh"

class LLVMTraceCPU;

class LLVMTraceCPUDriver final : public EmulatedDriver {
 public:
  LLVMTraceCPUDriver(LLVMTraceCPUDriverParams *p);
  void handshake(LLVMTraceCPU *llvm_trace_cpu);
  int open(Process *p, ThreadContext *tc, int mode, int flags) override;
  int ioctl(Process *p, ThreadContext *tc, unsigned req) override;

 private:
  LLVMTraceCPU *llvm_trace_cpu;

  enum IOCTL_REQUEST_CODE : unsigned long {
    IOCTL_REQUEST_REPLAY = 0,
  };
};

#endif