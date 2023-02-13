#include "insts.hh"

#include "base/trace.hh"
#include "cpu/gem_forge/accelerator/gem_forge_accelerator.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "debug/AbstractDataFlowAccelerator.hh"

namespace gem5 {

ADFAConfigInst::ADFAConfigInst(const LLVM::TDG::TDGInstruction &_TDG)
    : LLVMDynamicInst(_TDG, 1), finished(false) {
  if (!this->TDG.has_adfa_config()) {
    panic("ADFAConfigInst with missing protobuf field.");
  }
}

void ADFAConfigInst::execute(LLVMTraceCPU *cpu) {
  cpu->getAcceleratorManager()->handle(this);
}

void ADFAConfigInst::markFinished() {
  if (this->finished) {
    panic("This ADFAConfigInst %u is already marked finished.", this->getId());
  }
  this->finished = true;
}

ADFAStartInst::ADFAStartInst(const LLVM::TDG::TDGInstruction &_TDG,
                             std::shared_ptr<Buffer> _buffer)
    : LLVMDynamicInst(_TDG, 1), finished(false), buffer(_buffer) {}

void ADFAStartInst::execute(LLVMTraceCPU *cpu) {
  cpu->getAcceleratorManager()->handle(this);
}

void ADFAStartInst::markFinished() {
  if (this->finished) {
    panic("This ADFAStartInst %u is already marked finished.", this->getId());
  }
  this->finished = true;
}

LLVMDynamicInst *parseADFAInst(LLVM::TDG::TDGInstruction &TDGInst) {

  switch (TDGInst.extra_case()) {
  /**
   * ADFA instructions.
   */
  case LLVM::TDG::TDGInstruction::ExtraCase::kAdfaConfig: {
    DPRINTF(AbstractDataFlowAccelerator, "Parsed ADFAConfig inst.\n");
    return new ADFAConfigInst(TDGInst);
  }
  default: { break; }
  }

  /**
   * Some instructions do not have extra fields, so we use op to check them
   * directly.
   */
  if (TDGInst.op() == "df-start") {
    // DPRINTF(AbstractDataFlowAccelerator, "Parsed ADFAStart inst.\n");
    return new ADFAStartInst(TDGInst, nullptr);
  }

  /**
   * The end token is not an "real" instruction, just used to indicate that the
   * accelerator has finished processing one data flow. So to keep things
   * simple, we do not introduce another "ADFAEndToken" class.
   */

  return nullptr;
}} // namespace gem5

