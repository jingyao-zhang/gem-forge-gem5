#ifndef __CPU_LLVM_TRACE_CPU_THREAD_CONTEXT_HH__
#define __CPU_LLVM_TRACE_CPU_THREAD_CONTEXT_HH__

#include "dyn_inst_stream.hh"
#include "dyn_inst_stream_dispatcher.hh"
#include "region_stats.hh"

namespace gem5 {

/**
 * A basic thread context.
 */

class LLVMTraceCPU;

class LLVMTraceThreadContext {
public:
  LLVMTraceThreadContext(ContextID _contextId,
                         const std::string &_traceFileName,
                         bool _isIdeal = false, bool _enableADFA = false);
  virtual ~LLVMTraceThreadContext();

  /**
   * * Whether this should be modeled as an ideal thread.
   */
  bool isIdealThread() const { return this->isIdeal; }

  virtual void activate(LLVMTraceCPU *cpu, ThreadID threadId);
  virtual void deactivate();

  /**
   * ! A hack here to parse more instructions.
   */
  void parse() { this->dispatcher.parse(); }

  bool isActive() const { return this->cpu != nullptr; }
  virtual bool isDone() const;
  virtual bool canFetch() const;
  virtual LLVMDynamicInst *fetch();
  virtual void commit(LLVMDynamicInst *inst);

  const LLVM::TDG::StaticInformation &getStaticInfo() const {
    return this->dispatcher.getStaticInfo();
  }

  RegionStats *getRegionStats() { return this->regionStats; }

  ThreadID getThreadId() const {
    assert(this->isActive() &&
           "This context is not allocated hardware thread.");
    return this->threadId;
  }

protected:
  ContextID contextId;
  const std::string traceFileName;
  DynamicInstructionStreamDispatcher dispatcher;
  DynamicInstructionStream *dynInstStream;
  RegionStats *regionStats;

  const bool isIdeal;
  size_t inflyInsts;

  /**
   * States for active threads.
   */
  LLVMTraceCPU *cpu;
  ThreadID threadId;
};

} // namespace gem5

#endif