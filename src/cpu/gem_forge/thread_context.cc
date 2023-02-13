#include "thread_context.hh"

namespace gem5 {

LLVMTraceThreadContext::LLVMTraceThreadContext(
    ContextID _contextId, const std::string &_traceFileName, bool _isIdeal,
    bool _enableADFA)
    : contextId(_contextId), traceFileName(_traceFileName),
      dispatcher(_traceFileName, _enableADFA),
      dynInstStream(new DynamicInstructionStream(dispatcher.getMainBuffer())),
      regionStats(nullptr), isIdeal(_isIdeal), inflyInsts(0), cpu(nullptr),
      threadId(InvalidThreadID) {
  const bool enableRegionStats = true;
  if (enableRegionStats) {
    this->regionStats =
        new RegionStats(this->dispatcher.getRegionTable(), "region.stats.txt");
  }
}

LLVMTraceThreadContext::~LLVMTraceThreadContext() {
  delete this->dynInstStream;
  this->dynInstStream = nullptr;
  if (this->regionStats != nullptr) {
    delete this->regionStats;
    this->regionStats = nullptr;
  }
}

bool LLVMTraceThreadContext::canFetch() const {
  return !this->dynInstStream->fetchEmpty();
}

LLVMDynamicInst *LLVMTraceThreadContext::fetch() {
  assert(this->canFetch() && "Illega fetch.");
  this->inflyInsts++;
  return this->dynInstStream->fetch();
}

void LLVMTraceThreadContext::commit(LLVMDynamicInst *inst) {
  this->dynInstStream->commit(inst);
  this->inflyInsts--;
}

bool LLVMTraceThreadContext::isDone() const {
  return this->dynInstStream->fetchEmpty() && this->inflyInsts == 0;
}

void LLVMTraceThreadContext::activate(LLVMTraceCPU *cpu, ThreadID threadId) {
  this->cpu = cpu;
  this->threadId = threadId;
}

void LLVMTraceThreadContext::deactivate() {
  this->cpu = nullptr;
  this->threadId = InvalidThreadID;
}} // namespace gem5

