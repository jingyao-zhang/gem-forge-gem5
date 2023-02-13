#include "speculative_precomputation_manager.hh"
#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"
#include "debug/SpeculativePrecomputation.hh"
#include "insts.hh"

namespace gem5 {

SpeculativePrecomputationThread::SpeculativePrecomputationThread(
    ContextID _contextId, const std::string &_traceFileName, Addr _criticalPC)
    : LLVMTraceThreadContext(_contextId, _traceFileName,
                             true /* This is ideal thread. */),
      criticalPC(_criticalPC), tokens(0), numTriggeredSlices(0), numSlices(0) {
  const auto &staticInfo = this->getStaticInfo();
  assert(staticInfo.module() == "specpre" && "Unmatched module name.");
}

bool SpeculativePrecomputationThread::canFetch() const {
  return !this->dynInstStream->fetchEmpty() && this->tokens > 0;
}

LLVMDynamicInst *SpeculativePrecomputationThread::fetch() {
  assert(this->canFetch() && "Illega fetch.");
  this->inflyInsts++;
  auto inst = this->dynInstStream->fetch();
  if (inst->getTDG().pc() == this->criticalPC) {
    // We finished one slice.
    this->tokens--;
  }

  return inst;
}

bool SpeculativePrecomputationThread::isDone() const {
  return this->inflyInsts == 0 && this->tokens == 0;
}

void SpeculativePrecomputationThread::skipOneSlice() {
  assert(!this->isActive() && "Only skip when we are not active.");
  assert(this->tokens == 0 && "Only skip when we have no tokens.");
  this->tokens = 1;
  this->numSlices++;
  assert(this->canFetch() && "There should be a slice to skip.");
  while (this->canFetch()) {
    auto inst = this->fetch();
    this->commit(inst);
  }
  assert(this->tokens == 0 && "After skipping we should still have no token.");
  this->tokens = 0;
}

SpeculativePrecomputationManager::SpeculativePrecomputationManager(
    const Params &params)
    : GemForgeAccelerator(params) {}

SpeculativePrecomputationManager::~SpeculativePrecomputationManager() {
  for (auto &pcThread : this->criticalPCThreadMap) {
    delete pcThread.second;
    pcThread.second = nullptr;
  }
  this->criticalPCThreadMap.clear();
}

void SpeculativePrecomputationManager::handshake(
    GemForgeCPUDelegator *_cpuDelegator, GemForgeAcceleratorManager *_manager) {
  GemForgeAccelerator::handshake(_cpuDelegator, _manager);

  LLVMTraceCPU *_cpu = nullptr;
  if (auto llvmTraceCPUDelegator =
          dynamic_cast<LLVMTraceCPUDelegator *>(_cpuDelegator)) {
    _cpu = llvmTraceCPUDelegator->cpu;
  }
  assert(_cpu != nullptr && "Only work for LLVMTraceCPU so far.");
  this->cpu = _cpu;
}

void SpeculativePrecomputationManager::regStats() {
  GemForgeAccelerator::regStats();
  this->numTriggeredSlices
      .name(this->manager->name() + ".specpre.numTriggeredSlices")
      .desc("Number of slices triggered.")
      .prereq(this->numTriggeredSlices);
  this->numSkippedSlices
      .name(this->manager->name() + ".specpre.numSkippedSlices")
      .desc("Number of slices skipped.")
      .prereq(this->numSkippedSlices);
  this->numTriggeredChainSlices
      .name(this->manager->name() + ".specpre.numTriggeredChainSlices")
      .desc("Number of slices chaining triggered.")
      .prereq(this->numTriggeredChainSlices);
}

void SpeculativePrecomputationManager::tick() {}
bool SpeculativePrecomputationManager::handle(LLVMDynamicInst *inst) {
  return false;
}

void SpeculativePrecomputationManager::handleTrigger(
    SpeculativePrecomputationTriggerInst *inst) {
  const auto &info = inst->getTDG().specpre_trigger();
  auto criticalPC = info.critical_pc();
  if (this->criticalPCThreadMap.count(criticalPC) == 0) {
    auto sliceStreamFileName =
        cpuDelegator->getTraceExtraFolder() + "/" + info.slice_stream();
    this->criticalPCThreadMap.emplace(
        std::piecewise_construct, std::forward_as_tuple(criticalPC),
        std::forward_as_tuple(new SpeculativePrecomputationThread(
            cpu->allocateContextID(), sliceStreamFileName, criticalPC)));
  }
  auto thread = this->criticalPCThreadMap.at(criticalPC);

  if (thread->isActive()) {
    // Simply increase the token.
    thread->addToken();
    this->numTriggeredSlices++;
    this->numTriggeredChainSlices++;
  } else {
    // Check if the cpu still has available hardware context.
    if (cpu->getNumContexts() == cpu->getNumActiveThreads()) {
      // No free context. Skip it.
      thread->skipOneSlice();
      this->numSkippedSlices++;
    } else {
      // Activate the thread.
      thread->addToken();
      cpu->activateThread(thread);
      this->numTriggeredSlices++;
    }
  }
}

} // namespace gem5
