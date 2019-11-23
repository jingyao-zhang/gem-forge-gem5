#include "riscv_func_addr_callback.hh"

#include "../func_addr_exec_context.hh"
#include "arch/riscv/decoder.hh"
#include "base/loader/object_file.hh"
#include "base/loader/symtab.hh"
#include "cpu/exec_context.hh"
#include "debug/FuncAddrCallback.hh"
#include "sim/process.hh"

#define FUNC_ADDR_DPRINTF(format, args...)                                     \
  DPRINTF(FuncAddrCallback, "[%s]: " format, this->func.name().c_str(), ##args)

namespace {

/**
 * Since gem5 is single thread, and all the address computation is not
 * overlapped, we use a static global context.
 */
static AddrFuncExecContext addrFuncXC;

} // namespace

namespace RiscvISA {

FuncAddrGenCallback::FuncAddrGenCallback(ThreadContext *_tc,
                                         const ::LLVM::TDG::AddrFuncInfo &_func)
    : tc(_tc), func(_func), decoder(_tc->getDecoderPtr()), funcStartVAddr(0) {
  auto p = tc->getProcessPtr();
  auto obj = p->objFile;
  SymbolTable table;
  obj->loadAllSymbols(&table);
  assert(table.findAddress(this->func.name(), this->funcStartVAddr));
  FUNC_ADDR_DPRINTF("Start PC %#x.\n", this->funcStartVAddr);

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
    FUNC_ADDR_DPRINTF("Decode Inst %s.\n", staticInst->disassemble(pc).c_str());
    assert(!staticInst->isControl() &&
           "No control instruction allowed in address function.");
    this->instructions.push_back(staticInst);
    pc += sizeof(machInst);
  }
}

uint64_t FuncAddrGenCallback::genAddr(uint64_t idx,
                                      const std::vector<uint64_t> &params) {
  /**
   * Prepare the arguments according to the calling convention.
   */
  // a0 starts at x10.
  const RegIndex a0RegIdx = 10;
  auto argIdx = a0RegIdx;
  for (auto param : params) {
    RegId reg(RegClass::IntRegClass, argIdx);
    addrFuncXC.setIntRegOperand(reg, param);
    FUNC_ADDR_DPRINTF("Arg %d %llu.\n", argIdx - a0RegIdx, param);
    argIdx++;
  }

  for (auto &staticInst : this->instructions) {
    staticInst->execute(&addrFuncXC, nullptr /* traceData. */);
  }

  // The result value should be in a0 = x10.
  RegId a0Reg(RegClass::IntRegClass, a0RegIdx);
  auto retAddr = addrFuncXC.readIntRegOperand(a0Reg);
  FUNC_ADDR_DPRINTF("Ret %llu.\n", retAddr);
  return retAddr;
}
} // namespace RiscvISA