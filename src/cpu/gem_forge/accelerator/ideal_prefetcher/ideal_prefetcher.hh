#ifndef __CPU_GEM_FORGE_ACCELERATOR_IDEAL_PREFETCHER_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_IDEAL_PREFETCHER_HH__

#include "cpu/gem_forge/TDGInstruction.pb.h"
#include "cpu/gem_forge/accelerator/gem_forge_accelerator.hh"
#include "cpu/gem_forge/gem_forge_packet_handler.hh"

#include "params/IdealPrefetcher.hh"

namespace gem5 {

class IdealPrefetcher : public GemForgeAccelerator,
                        public GemForgePacketHandler {
public:
  PARAMS(IdealPrefetcher)
  IdealPrefetcher(const Params &params);

  void handshake(GemForgeCPUDelegator *_cpuDelegator,
                 GemForgeAcceleratorManager *_manager) override;
  bool handle(LLVMDynamicInst *inst) override;
  void tick() override;
  void dump() override;
  void regStats() override;

  // Prefetcher does not care about the result.
  void handlePacketResponse(GemForgeCPUDelegator *cpuDelegator,
                            PacketPtr packet) override {
    delete packet;
  }
  void issueToMemoryCallback(GemForgeCPUDelegator *cpuDelegator) override {}

private:
  LLVMTraceCPU *cpu;
  LLVM::TDG::CacheWarmUp cacheWarmUpProto;
  size_t prefetchedIdx;
  bool enabled;
  uint32_t prefetchDistance;
};

} // namespace gem5

#endif