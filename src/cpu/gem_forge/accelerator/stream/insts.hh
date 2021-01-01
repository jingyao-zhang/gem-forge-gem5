#ifndef __CPU_TDG_ACCELERATOR_STREAM_INST_H__
#define __CPU_TDG_ACCELERATOR_STREAM_INST_H__

#include "cpu/gem_forge/llvm_insts.hh"

class StreamInst : public LLVMDynamicInst {
public:
  StreamInst(const LLVM::TDG::TDGInstruction &_TDG);
  bool isCompleted() const override { return this->finished; }
  void markFinished();

  virtual uint64_t getStreamId() const = 0;

protected:
  bool finished;
};

class StreamConfigInst : public StreamInst {
public:
  StreamConfigInst(const LLVM::TDG::TDGInstruction &_TDG);
  bool canDispatch(LLVMTraceCPU *cpu) const override;
  void dispatch(LLVMTraceCPU *cpu) override;
  void execute(LLVMTraceCPU *cpu) override;
  void commit(LLVMTraceCPU *cpu) override;
  uint64_t getStreamId() const override;
  void dumpBasic() const override;
};

class StreamStepInst : public StreamInst {
public:
  StreamStepInst(const LLVM::TDG::TDGInstruction &_TDG);

  bool canDispatch(LLVMTraceCPU *cpu) const override;
  void dispatch(LLVMTraceCPU *cpu) override;
  void execute(LLVMTraceCPU *cpu) override;
  void commit(LLVMTraceCPU *cpu) override;
  uint64_t getStreamId() const override;
  void dumpBasic() const override;
};

class StreamStoreInst : public StreamInst {
public:
  StreamStoreInst(const LLVM::TDG::TDGInstruction &_TDG);
  bool canDispatch(LLVMTraceCPU *cpu) const override;
  
  int getNumSQEntries(LLVMTraceCPU *cpu) const override { return 1; }
  std::list<std::unique_ptr<GemForgeSQDeprecatedCallback>>
  createAdditionalSQCallbacks(LLVMTraceCPU *cpu) override;

  void dispatch(LLVMTraceCPU *cpu) override;
  void execute(LLVMTraceCPU *cpu) override;
  void commit(LLVMTraceCPU *cpu) override;
  uint64_t getStreamId() const override;
};

class StreamEndInst : public StreamInst {
public:
  StreamEndInst(const LLVM::TDG::TDGInstruction &_TDG);
  void dispatch(LLVMTraceCPU *cpu) override;
  void execute(LLVMTraceCPU *cpu) override;
  void commit(LLVMTraceCPU *cpu) override;
  uint64_t getStreamId() const override;
};

/**
 * Helper function used by DynamicInstructionStream to parse the TDGInstruction.
 * Returns nullptr if this is not stream instruction.
 */

LLVMDynamicInst *parseStreamInst(LLVM::TDG::TDGInstruction &TDGInst);

#endif