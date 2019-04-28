#ifndef __CPU_TDG_ACCELERATOR_SPECULATIVE_PRECOMPUTATION_MANAGER_HH__
#define __CPU_TDG_ACCELERATOR_SPECULATIVE_PRECOMPUTATION_MANAGER_HH__

class SpeculativePrecomputationTriggerInst;

#include "base/statistics.hh"
#include "cpu/gem_forge/accelerator/tdg_accelerator.hh"
#include "cpu/gem_forge/thread_context.hh"

#include <unordered_map>

class SpeculativePrecomputationThread : public LLVMTraceThreadContext {
public:
  SpeculativePrecomputationThread(ThreadID _threadId,
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

class SpeculativePrecomputationManager : public TDGAccelerator {
public:
  SpeculativePrecomputationManager();
  ~SpeculativePrecomputationManager() override;

  void handshake(LLVMTraceCPU *_cpu, TDGAcceleratorManager *_manager) override;

  void handleTrigger(SpeculativePrecomputationTriggerInst *inst);
  void tick() override;
  bool handle(LLVMDynamicInst *inst) override;

  void regStats() override;

  Stats::Scalar numTriggeredSlices;
  Stats::Scalar numSkippedSlices;
  Stats::Scalar numTriggeredChainSlices;

private:
  std::unordered_map<Addr, SpeculativePrecomputationThread *>
      criticalPCThreadMap;
};

#endif
