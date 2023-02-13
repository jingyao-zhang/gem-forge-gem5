
#ifndef __CPU_TDG_ACCELERATOR_ADFA_INST_H__
#define __CPU_TDG_ACCELERATOR_ADFA_INST_H__

#include "cpu/gem_forge/llvm_insts.hh"
#include "cpu/gem_forge/queue_buffer.hh"

namespace gem5 {

class ADFAConfigInst : public LLVMDynamicInst {
public:
  ADFAConfigInst(const LLVM::TDG::TDGInstruction &_TDG);

  void execute(LLVMTraceCPU *cpu) override;
  bool isCompleted() const override { return this->finished; }

  /**
   * ADFAConfigInst is an serialization point.
   */
  bool isSerializeBefore() const override { return true; }
  bool isSerializeAfter() const override { return true; }

  /**
   * Interface for AbstractDataFlowAccelerator.
   */
  void markFinished();

private:
  bool finished;
};

/**
 * Start the accelerator, and block until it finishes.
 */
class ADFAStartInst : public LLVMDynamicInst {
public:
  // Stores the instruction stream buffer.
  using Packet = DynamicInstructionStreamPacket;
  using Buffer = QueueBuffer<Packet>;

  ADFAStartInst(const LLVM::TDG::TDGInstruction &_TDG,
                std::shared_ptr<Buffer> _buffer);

  std::shared_ptr<Buffer> getBuffer() { return this->buffer; }

  void execute(LLVMTraceCPU *cpu) override;
  bool isCompleted() const override { return this->finished; }

  /**
   * ADFAStartInst is an serialization point.
   */
  bool isSerializeBefore() const override { return true; }
  bool isSerializeAfter() const override { return true; }

  /**
   * Interface for AbstractDataFlowAccelerator.
   */
  void markFinished();

private:
  bool finished;
  std::shared_ptr<Buffer> buffer;
};

/**
 * Helper function used by DynamicInstructionStream to parse the TDGInstruction.
 * Returns nullptr if this is not ADFA instruction.
 */
LLVMDynamicInst *parseADFAInst(LLVM::TDG::TDGInstruction &TDGInst);

} // namespace gem5

#endif