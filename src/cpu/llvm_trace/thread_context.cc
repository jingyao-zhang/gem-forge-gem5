#include "thread_context.hh"

LLVMTraceThreadContext::LLVMTraceThreadContext(
    ThreadID _threadId, const std::string &_traceFileName)
    : threadId(_threadId),
      dynInstStream(new DynamicInstructionStream(_traceFileName)),
      inflyInsts(0) {}

LLVMTraceThreadContext::~LLVMTraceThreadContext() {
  delete this->dynInstStream;
  this->dynInstStream = nullptr;
}

bool LLVMTraceThreadContext::canFetch() const {
  return !this->dynInstStream->fetchEmpty();
}

void LLVMDynamicInst *LLVMTraceThreadContext::fetch() {
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