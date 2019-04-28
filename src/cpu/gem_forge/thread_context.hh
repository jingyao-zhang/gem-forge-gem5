#ifndef __CPU_LLVM_TRACE_CPU_THREAD_CONTEXT_HH__
#define __CPU_LLVM_TRACE_CPU_THREAD_CONTEXT_HH__

#include "dyn_inst_stream.hh"

/**
 * A basic thread context.
 */

class LLVMTraceCPU;

class LLVMTraceThreadContext {
public:
  LLVMTraceThreadContext(ThreadID _threadId, const std::string &_traceFileName);
  virtual ~LLVMTraceThreadContext();

  virtual void activate(LLVMTraceCPU *cpu, ThreadID contextId);
  virtual void deactivate();

  bool isActive() const { return this->cpu != nullptr; }
  virtual bool isDone() const;
  virtual bool canFetch() const;
  virtual LLVMDynamicInst *fetch();
  virtual void commit(LLVMDynamicInst *inst);

  const LLVM::TDG::StaticInformation &getStaticInfo() const {
    return this->dynInstStream->getStaticInfo();
  }

  ThreadID getContextId() const {
    assert(this->isActive() &&
           "This thread is not allocated hardware context.");
    return this->contextId;
  }

protected:
  ThreadID threadId;
  const std::string traceFileName;
  DynamicInstructionStream *dynInstStream;
  size_t inflyInsts;

  /**
   * States for active threads.
   */
  LLVMTraceCPU *cpu;
  ThreadID contextId;
};

#endif