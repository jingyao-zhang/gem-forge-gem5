#include "llvm_trace_cpu_driver.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "cpu/thread_context.hh"
#include "debug/LLVMTraceCPU.hh"
#include "sim/process.hh"

#include <utility>
#include <vector>

namespace {} // namespace

LLVMTraceCPUDriver::LLVMTraceCPUDriver(LLVMTraceCPUDriverParams *p)
    : EmulatedDriver(p), llvm_trace_cpu(nullptr) {}

void LLVMTraceCPUDriver::handshake(LLVMTraceCPU *llvm_trace_cpu) {
  DPRINTF(LLVMTraceCPU, "driver received handshake from %s\n",
          llvm_trace_cpu->name().c_str());
  this->llvm_trace_cpu = llvm_trace_cpu;
}

int LLVMTraceCPUDriver::open(ThreadContext *tc, int mode, int flags) {
  auto p = tc->getProcessPtr();
  static int tgt_fd = -1;
  if (tgt_fd == -1) {
    std::shared_ptr<DeviceFDEntry> fdp;
    fdp = std::make_shared<DeviceFDEntry>(this, filename);
    tgt_fd = p->fds->allocFD(fdp);
  }
  DPRINTF(LLVMTraceCPU, "open called return fd %d\n", tgt_fd);
  return tgt_fd;
}

int LLVMTraceCPUDriver::ioctl(ThreadContext *tc, unsigned req, Addr buf) {
  DPRINTF(LLVMTraceCPU, "ioctl called with req %u\n", req);
  auto &memProxy = tc->getVirtProxy();
  auto p = tc->getProcessPtr();
  switch (req) {
  case IOCTL_REQUEST_REPLAY: {
    // Deserialize the arguments.
    const int NUM_ARGS = 23;
    uint64_t args[NUM_ARGS];
    memProxy.readBlob(buf, reinterpret_cast<uint8_t *>(args),
                      NUM_ARGS * sizeof(args[0]));
    Addr trace_vaddr = reinterpret_cast<Addr>(args[0]);
    std::string trace;
    memProxy.readString(trace, trace_vaddr);
    Addr finish_tag_vaddr = reinterpret_cast<Addr>(args[1]);
    uint64_t num_maps = args[2];
    std::vector<std::pair<std::string, Addr>> maps;

    for (uint64_t i = 0; i < num_maps; ++i) {
      std::string base;
      Addr base_vaddr = reinterpret_cast<Addr>(args[i * 2 + 3]);
      memProxy.readString(base, base_vaddr);
      Addr vaddr = reinterpret_cast<Addr>(args[i * 2 + 4]);
      maps.emplace_back(base, vaddr);
    }

    this->llvm_trace_cpu->handleReplay(p, tc, trace, finish_tag_vaddr,
                                       std::move(maps));
    break;
  }
  default: {
    panic("Unknown request code: %u\n", req);
  }
  }
  return 0;
}

void LLVMTraceCPUDriver::map(Process *p, ThreadContext *tc, Addr base_ptr,
                             Addr vaddr) {
  auto &memProxy = tc->getVirtProxy();
  std::string base;
  memProxy.readString(base, base_ptr);
  this->llvm_trace_cpu->mapBaseNameToVAddr(base, vaddr);
}

void LLVMTraceCPUDriver::replay(Process *p, ThreadContext *tc, Addr trace_ptr,
                                Addr vaddr) {
  auto &memProxy = tc->getVirtProxy();
  std::string trace;
  memProxy.readString(trace, trace_ptr);
  // This is the API for pseudo instructions,
  // so no mapping here.
  std::vector<std::pair<std::string, Addr>> empty_maps;
  this->llvm_trace_cpu->handleReplay(p, tc, trace, vaddr, empty_maps);
}

LLVMTraceCPUDriver *LLVMTraceCPUDriverParams::create() {
  return new LLVMTraceCPUDriver(this);
}
