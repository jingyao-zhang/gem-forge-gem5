#include "ideal_prefetcher.hh"

#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"
#include "debug/IdealPrefetcher.hh"

namespace gem5 {

IdealPrefetcher::IdealPrefetcher(const Params &params)
    : GemForgeAccelerator(params), prefetchedIdx(0), enabled(false),
      prefetchDistance(0) {
  this->enabled = params.enableIdealPrefetcher;
  this->prefetchDistance = params.idealPrefetcherDistance;
}

void IdealPrefetcher::handshake(GemForgeCPUDelegator *_cpuDelegator,
                                GemForgeAcceleratorManager *_manager) {
  GemForgeAccelerator::handshake(_cpuDelegator, _manager);

  LLVMTraceCPU *_cpu = nullptr;
  if (auto llvmTraceCPUDelegator =
          dynamic_cast<LLVMTraceCPUDelegator *>(_cpuDelegator)) {
    _cpu = llvmTraceCPUDelegator->cpu;
  }
  assert(_cpu != nullptr && "Only work for LLVMTraceCPU so far.");
  this->cpu = _cpu;

  if (this->enabled) {
    // Load the cache warm up file.
    ProtoInputStream is(cpu->getTraceFileName() + ".cache");
    assert(is.read(this->cacheWarmUpProto) &&
           "Failed to read in the history for ideal prefetcher.");
  }
}

void IdealPrefetcher::regStats() { GemForgeAccelerator::regStats(); }

bool IdealPrefetcher::handle(LLVMDynamicInst *inst) {
  // Ideal prefetcher is not based on instruction.
  return false;
}

void IdealPrefetcher::dump() {}

void IdealPrefetcher::tick() {
  if (!this->enabled) {
    return;
  }
  const auto robHeadInstId = cpu->getIEWStage().getROBHeadInstId();
  if (robHeadInstId == LLVMDynamicInst::INVALID_INST_ID) {
    return;
  }
  auto robHeadInst = cpu->getInflyInst(robHeadInstId);
  while (this->prefetchedIdx < this->cacheWarmUpProto.requests_size()) {
    const auto &request = this->cacheWarmUpProto.requests(this->prefetchedIdx);
    const auto seq = request.seq();
    if (robHeadInst->getSeqNum() + 100 >= seq) {
      // We are too close instruction ahead of the rob head.
      // It is unlikely that prefetch can help.
      // Skip this one.
      this->prefetchedIdx++;
      continue;
    }

    if (robHeadInst->getSeqNum() + this->prefetchDistance <= seq) {
      // We are too ahead. Do not prefetch now.
      break;
    }

    // We should prefetch.
    const auto vaddr = request.addr();
    auto size = request.size();
    const auto pc = request.pc();

    // Truncate by cache line.
    const auto cacheLineSize = this->cpuDelegator->cacheLineSize();
    if ((vaddr % cacheLineSize) + size > cacheLineSize) {
      size = cacheLineSize - (vaddr % cacheLineSize);
    }

    Addr paddr;
    if (!this->cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
      panic("Failed translate vaddr %#x.\n", vaddr);
    }
    auto pkt = GemForgePacketHandler::createGemForgePacket(
        paddr, size, this, nullptr, this->cpuDelegator->dataRequestorId(), 0, pc);

    this->cpuDelegator->sendRequest(pkt);

    this->prefetchedIdx++;
    // Only one prefetch per cycle.
    break;
  }
}

} // namespace gem5

