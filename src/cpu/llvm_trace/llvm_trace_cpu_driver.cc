#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "cpu/thread_context.hh"
#include "debug/LLVMTraceCPU.hh"
#include "llvm_trace_cpu_driver.hh"
#include "sim/process.hh"

namespace {}  // namespace

LLVMTraceCPUDriver::LLVMTraceCPUDriver(LLVMTraceCPUDriverParams* p)
    : EmulatedDriver(p), llvm_trace_cpu(nullptr) {}

void LLVMTraceCPUDriver::handshake(LLVMTraceCPU* llvm_trace_cpu) {
  DPRINTF(LLVMTraceCPU, "driver received handshake from %s\n",
          llvm_trace_cpu->name().c_str());
  this->llvm_trace_cpu = llvm_trace_cpu;
}

int LLVMTraceCPUDriver::open(Process* p, ThreadContext* tc, int mode,
                             int flags) {
  static int tgt_fd = -1;
  if (tgt_fd == -1) {
    std::shared_ptr<DeviceFDEntry> fdp;
    fdp = std::make_shared<DeviceFDEntry>(this, filename);
    tgt_fd = p->fds->allocFD(fdp);
  }
  DPRINTF(LLVMTraceCPU, "open called return fd %d\n", tgt_fd);
  return tgt_fd;
}

int LLVMTraceCPUDriver::ioctl(Process* p, ThreadContext* tc, unsigned req) {
  DPRINTF(LLVMTraceCPU, "ioctl called with req %u\n", req);
  SETranslatingPortProxy& memProxy = tc->getMemProxy();
  switch (req) {
    case IOCTL_REQUEST_MAP: {
      int index = 2;
      Addr args_vaddr = p->getSyscallArg(tc, index);
      // Deserialize the arguments.
      const int NUM_ARGS = 2;
      uint64_t args[NUM_ARGS];
      memProxy.readBlob(args_vaddr, reinterpret_cast<uint8_t*>(args),
                        NUM_ARGS * sizeof(args[0]));
      Addr base_vaddr = reinterpret_cast<Addr>(args[0]);
      std::string base;
      memProxy.readString(base, base_vaddr);
      Addr vaddr = reinterpret_cast<Addr>(args[1]);

      this->llvm_trace_cpu->handleMapVirtualMem(p, tc, base, vaddr);
      break;
    }
    case IOCTL_REQUEST_REPLAY: {
      int index = 2;
      Addr args_vaddr = p->getSyscallArg(tc, index);
      // Deserialize the arguments.
      const int NUM_ARGS = 2;
      uint64_t args[NUM_ARGS];
      memProxy.readBlob(args_vaddr, reinterpret_cast<uint8_t*>(args),
                        NUM_ARGS * sizeof(args[0]));
      Addr trace_vaddr = reinterpret_cast<Addr>(args[0]);
      std::string trace;
      memProxy.readString(trace, trace_vaddr);
      Addr finish_tag_vaddr = reinterpret_cast<Addr>(args[1]);

      this->llvm_trace_cpu->handleReplay(p, tc, trace, finish_tag_vaddr);
      break;
    }
    default: { panic("Unknown request code: %u\n", req); }
  }
  return 0;
}

LLVMTraceCPUDriver* LLVMTraceCPUDriverParams::create() {
  return new LLVMTraceCPUDriver(this);
}
