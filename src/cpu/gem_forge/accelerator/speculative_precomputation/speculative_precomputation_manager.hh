#ifndef __CPU_TDG_ACCELERATOR_SPECULATIVE_PRECOMPUTATION_MANAGER_HH__
#define __CPU_TDG_ACCELERATOR_SPECULATIVE_PRECOMPUTATION_MANAGER_HH__

class SpeculativePrecomputationTriggerInst;

#include "base/statistics.hh"
#include "cpu/gem_forge/accelerator/gem_forge_accelerator.hh"
#include "cpu/gem_forge/thread_context.hh"

#include "params/SpeculativePrecomputationManager.hh"

#include <unordered_map>

namespace gem5 {

class SpeculativePrecomputationTriggerInst;

class SpeculativePrecomputationThread : public LLVMTraceThreadContext {
public:
  SpeculativePrecomputationThread(ContextID _contextId,
                                  const std::string &_traceFileName,
                                  Addr _criticalPC);

  bool isDone() const override;
  bool canFetch() const override;
  LLVMDynamicInst *fetch() override;

  /**
   * When there is no available hardware context,
   * we skip one slice from the stream.
   */
  void skipOneSlice();

  /**
   * Increase the number of tokens.
   */
  void addToken() {
    this->tokens++;
    this->numTriggeredSlices++;
    this->numSlices++;
  }

private:
  Addr criticalPC;
  size_t tokens;
  size_t numTriggeredSlices;
  size_t numSlices;
};

class SpeculativePrecomputationManager : public GemForgeAccelerator {
public:
  PARAMS(SpeculativePrecomputationManager)
  SpeculativePrecomputationManager(const Params &params);
  ~SpeculativePrecomputationManager() override;

  void handshake(GemForgeCPUDelegator *_cpuDelegator,
                 GemForgeAcceleratorManager *_manager) override;

  void handleTrigger(SpeculativePrecomputationTriggerInst *inst);
  void tick() override;
  bool handle(LLVMDynamicInst *inst) override;

  void regStats() override;

  statistics::Scalar numTriggeredSlices;
  statistics::Scalar numSkippedSlices;
  statistics::Scalar numTriggeredChainSlices;

private:
  LLVMTraceCPU *cpu;
  std::unordered_map<Addr, SpeculativePrecomputationThread *>
      criticalPCThreadMap;
};

} // namespace gem5

#endif
