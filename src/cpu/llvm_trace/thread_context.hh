#ifndef __CPU_LLVM_TRACE_CPU_THREAD_CONTEXT_HH__
#define __CPU_LLVM_TRACE_CPU_THREAD_CONTEXT_HH__

#include "dyn_inst_stream.hh"

/**
 * A basic thread context.
 */

class LLVMTraceThreadContext {
public:
  LLVMTraceThreadContext(ThreadID _threadId, const std::string &_traceFileName);
  ~LLVMTraceThreadContext();

  virtual bool isDone() const;

  virtual bool canFetch() const;
  virtual LLVMDynamicInst *fetch();
  virtual void commit(LLVMDynamicInst *inst);

  const LLVM::TDG::StaticInformation &getStaticInfo() const {
    return this->dynInstStream->getStaticInfo();
  }

private:
  ThreadID threadId;
  DynamicInstructionStream *dynInstStream;
  size_t inflyInsts;
};

#endif