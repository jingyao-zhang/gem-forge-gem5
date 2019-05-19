#ifndef __CPU_GEM_FORGE_ACCELERATOR_IDEAL_PREFETCHER_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_IDEAL_PREFETCHER_HH__

#include "cpu/gem_forge/TDGInstruction.pb.h"
#include "cpu/gem_forge/accelerator/tdg_accelerator.hh"
#include "cpu/gem_forge/tdg_packet_handler.hh"

class IdealPrefetcher : public TDGAccelerator, public TDGPacketHandler {
public:
  IdealPrefetcher();

  void handshake(LLVMTraceCPU *_cpu, TDGAcceleratorManager *_manager) override;
  bool handle(LLVMDynamicInst *inst) override;
  void tick() override;
  void dump() override;
  void regStats() override;

  // Prefetcher does not care about the result.
  void handlePacketResponse(LLVMTraceCPU *cpu, PacketPtr packet) override {
    delete packet;
  }

private:
  LLVM::TDG::CacheWarmUp cacheWarmUpProto;
  size_t prefetchedIdx;
  bool enabled;
  uint32_t prefetchDistance;
};

#endif