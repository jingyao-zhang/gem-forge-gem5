#ifndef __CPU_LLVM_TRACE_LLVM_STATIC_INST_HH__
#define __CPU_LLVM_TRACE_LLVM_STATIC_INST_HH__

#include "llvm_insts.hh"

#include "cpu/static_inst.hh"

/**
 * Dummy class only used for branch prediction.
 */

class LLVMStaticInst : public StaticInst {
public:
  LLVMStaticInst(const LLVMDynamicInst *_dynamicInst)
      : StaticInst("", ExtMachInst(), OpClass::No_OpClass),
        dynamicInst(_dynamicInst) {
    assert(this->dynamicInst->isBranchInst() &&
           "Only static inst for conditional branch so far.");
    this->flags.set(IsControl);
    const auto &branch = this->dynamicInst->getTDG().branch();
    /**
     * From gem5's branch predictor stats, it seems that this is not correctly
     * set. BP lookups is always the same as BP conditional predicted.
     *
     * So I took the different approach by setting conditional flag.
     */
    if (branch.is_conditional()) {
      this->flags.set(IsCondControl);
    } else {
      this->flags.set(IsUncondControl);
    }
    if (branch.is_indirect()) {
      this->flags.set(IsIndirectControl);
    } else {
      this->flags.set(IsDirectControl);
    }
    const auto &op = this->dynamicInst->getInstName();
    if (op == "ret") {
      this->flags.set(IsReturn);
    } else if (op == "call" || op == "invoke") {
      this->flags.set(IsCall);
    }
  }

  Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override {
    panic("execute not defined!");
  }

  std::string
  generateDisassembly(Addr pc,
                      const ::Loader::SymbolTable *symtab) const override {
    panic("generateDisassembly not defined!");
  }

  void advancePC(TheISA::PCState &pcState) const override {
    pcState.set(this->dynamicInst->getStaticNextPC());
  }

private:
  const LLVMDynamicInst *dynamicInst;
};

#endif