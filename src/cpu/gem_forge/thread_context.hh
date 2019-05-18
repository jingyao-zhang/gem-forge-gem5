#ifndef __CPU_LLVM_TRACE_CPU_THREAD_CONTEXT_HH__
#define __CPU_LLVM_TRACE_CPU_THREAD_CONTEXT_HH__

#include "dyn_inst_stream.hh"

/**
 * A basic thread context.
 */

class LLVMTraceCPU;

class LLVMTraceThreadContext {
public:
  LLVMTraceThreadContext(ContextID _contextId,
                         const std::string &_traceFileName,
                         bool _isIdeal = false);
  virtual ~LLVMTraceThreadContext();

  /**
   * * Whether this should be modeled as an ideal thread.
   */
  bool isIdealThread() const { return this->isIdeal; }

  virtual void activate(LLVMTraceCPU *cpu, ThreadID threadId);
  virtual void deactivate();

  bool isActive() const { return this->cpu != nullptr; }
  virtual bool isDone() const;
  virtual bool canFetch() const;
  virtual LLVMDynamicInst *fetch();
  virtual void commit(LLVMDynamicInst *inst);

  const LLVM::TDG::StaticInformation &getStaticInfo() const {
    return this->dynInstStream->getStaticInfo();
  }

  ThreadID getThreadId() const {
    assert(this->isActive() &&
           "This context is not allocated hardware thread.");
    return this->threadId;
  }

protected:
  ContextID contextId;
  const std::string traceFileName;
  DynamicInstructionStream *dynInstStream;
  const bool isIdeal;
  size_t inflyInsts;

  /**
   * States for active threads.
   */
  LLVMTraceCPU *cpu;
  ThreadID threadId;
};

#endif