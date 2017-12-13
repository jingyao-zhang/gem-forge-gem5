#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "cpu/thread_context.hh"
#include "debug/LLVMTraceCPU.hh"
#include "llvm_trace_cpu_driver.hh"
#include "sim/process.hh"

LLVMTraceCPUDriver::LLVMTraceCPUDriver(LLVMTraceCPUDriverParams* p)
    : EmulatedDriver(p), llvm_trace_cpu(nullptr) {}

void LLVMTraceCPUDriver::handshake(LLVMTraceCPU* llvm_trace_cpu) {
  DPRINTF(LLVMTraceCPU, "driver received handshake from %s\n",
          llvm_trace_cpu->name().c_str());
  this->llvm_trace_cpu = llvm_trace_cpu;
}

int LLVMTraceCPUDriver::open(Process* p, ThreadContext* tc, int mode,
                             int flags) {
  std::shared_ptr<DeviceFDEntry> fdp;
  fdp = std::make_shared<DeviceFDEntry>(this, filename);
  int tgt_fd = p->fds->allocFD(fdp);
  DPRINTF(LLVMTraceCPU, "open called return fd %d\n", tgt_fd);
  return tgt_fd;
}

int LLVMTraceCPUDriver::ioctl(Process* p, ThreadContext* tc, unsigned req) {
  DPRINTF(LLVMTraceCPU, "ioctl called\n");
  return 0;
}

LLVMTraceCPUDriver* LLVMTraceCPUDriverParams::create() {
  return new LLVMTraceCPUDriver(this);
}
