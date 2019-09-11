#include "ideal_prefetcher.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "debug/IdealPrefetcher.hh"

IdealPrefetcher::IdealPrefetcher(Params *params)
    : GemForgeAccelerator(params), prefetchedIdx(0), enabled(false),
      prefetchDistance(0) {
  this->enabled = params->enableIdealPrefetcher;
  this->prefetchDistance = params->idealPrefetcherDistance;
}

void IdealPrefetcher::handshake(LLVMTraceCPU *_cpu,
                                GemForgeAcceleratorManager *_manager) {
  GemForgeAccelerator::handshake(_cpu, _manager);
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
    const auto cacheLineSize = this->cpu->system->cacheLineSize();
    if ((vaddr % cacheLineSize) + size > cacheLineSize) {
      size = cacheLineSize - (vaddr % cacheLineSize);
    }

    auto paddr = this->cpu->translateAndAllocatePhysMem(vaddr);
    auto pkt = TDGPacketHandler::createTDGPacket(
        paddr, size, this, nullptr, this->cpu->getDataMasterID(), 0, pc);

    this->cpu->sendRequest(pkt);

    this->prefetchedIdx++;
    // Only one prefetch per cycle.
    break;
  }
}

IdealPrefetcher *IdealPrefetcherParams::create() {
  return new IdealPrefetcher(this);
}
