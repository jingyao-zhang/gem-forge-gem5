#include "riscv_exec_func.hh"

#include "../exec_func_context.hh"
#include "arch/riscv/decoder.hh"
#include "base/loader/object_file.hh"
#include "base/loader/symtab.hh"
#include "cpu/exec_context.hh"
#include "debug/ExecFunc.hh"
#include "sim/process.hh"

#define EXEC_FUNC_DPRINTF(format, args...)                                     \
  DPRINTF(ExecFunc, "[%s]: " format, this->func.name().c_str(), ##args)

namespace {

/**
 * Since gem5 is single thread, and all the address computation is not
 * overlapped, we use a static global context.
 */
static ExecFuncContext execFuncXC;

} // namespace

namespace RiscvISA {

ExecFunc::ExecFunc(ThreadContext *_tc, const ::LLVM::TDG::ExecFuncInfo &_func)
    : tc(_tc), func(_func), decoder(_tc->getDecoderPtr()), funcStartVAddr(0) {
  auto p = tc->getProcessPtr();
  auto obj = p->objFile;
  SymbolTable table;
  obj->loadAllSymbols(&table);
  assert(table.findAddress(this->func.name(), this->funcStartVAddr));
  EXEC_FUNC_DPRINTF("Start PC %#x.\n", this->funcStartVAddr);

  auto &prox = this->tc->getVirtProxy();
  auto pc = this->funcStartVAddr;

  while (true) {
    MachInst machInst;
    // Read the instructions.
    assert(prox.tryReadBlob(pc, &machInst, sizeof(machInst)) &&
           "Failed to read in instruction.");
    // Use the stateless decodeInst function.
    auto staticInst = this->decoder->decodeInst(machInst);

    if (staticInst->isCall()) {
      /**
       * Though wierd, RISCV jalr instruction is marked as
       * IsIndirectControl, IsUncondControl and IsCall.
       * We use IsCall to check if we are done.
       */
      break;
    }
    // We assume there is no branch.
    EXEC_FUNC_DPRINTF("Decode Inst %s.\n", staticInst->disassemble(pc).c_str());
    assert(!staticInst->isControl() &&
           "No control instruction allowed in address function.");
    this->instructions.push_back(staticInst);
    pc += sizeof(machInst);
  }
}

uint64_t ExecFunc::invoke(const std::vector<uint64_t> &params) {
  assert(params.size() == this->func.args_size());
  execFuncXC.clear();
  /**
   * Prepare the arguments according to the calling convention.
   */
  // a0 starts at x10.
  const RegIndex a0RegIdx = 10;
  auto argIdx = a0RegIdx;
  for (auto idx = 0; idx < params.size(); ++idx) {
    auto param = params.at(idx);
    assert(!this->func.args(idx).is_float() &&
           "Do not know how to handle float param");
    RegId reg(RegClass::IntRegClass, argIdx);
    execFuncXC.setIntRegOperand(reg, param);
    EXEC_FUNC_DPRINTF("Arg %d %llu.\n", argIdx - a0RegIdx, param);
    argIdx++;
  }

  for (auto &staticInst : this->instructions) {
    staticInst->execute(&execFuncXC, nullptr /* traceData. */);
  }

  // The result value should be in a0 = x10.
  assert(!this->func.is_float() && "Do not support float return value yet.");
  RegId a0Reg(RegClass::IntRegClass, a0RegIdx);
  auto retAddr = execFuncXC.readIntRegOperand(a0Reg);
  EXEC_FUNC_DPRINTF("Ret %llu.\n", retAddr);
  return retAddr;
}
} // namespace RiscvISA