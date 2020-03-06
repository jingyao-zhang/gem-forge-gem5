#include "x86_exec_func.hh"

#include "../exec_func_context.hh"
#include "arch/x86/decoder.hh"
#include "arch/x86/insts/macroop.hh"
#include "arch/x86/regs/float.hh"
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

namespace X86ISA {

ExecFunc::ExecFunc(ThreadContext *_tc, const ::LLVM::TDG::ExecFuncInfo &_func)
    : tc(_tc), func(_func) {
  auto p = tc->getProcessPtr();
  auto obj = p->objFile;
  SymbolTable table;
  obj->loadAllSymbols(&table);
  Addr funcStartVAddr;
  if (!table.findAddress(this->func.name(), funcStartVAddr)) {
    panic("Failed to resovle symbol: %s.", this->func.name());
  }

  auto dsEffBase = tc->readMiscRegNoEffect(MiscRegIndex::MISCREG_DS_EFF_BASE);
  auto csEffBase = tc->readMiscRegNoEffect(MiscRegIndex::MISCREG_CS_EFF_BASE);
  assert(dsEffBase == 0 && "We cannot handle this DS.\n");
  assert(csEffBase == 0 && "We cannot handle this CS.\n");

  EXEC_FUNC_DPRINTF("======= Start decoding from %#x.\n", funcStartVAddr);

  auto &prox = this->tc->getVirtProxy();

  /**
   * Let's create a new decoder to avoid interfering the original one's state.
   */
  TheISA::Decoder decoder;
  decoder.takeOverFrom(this->tc->getDecoderPtr());
  decoder.reset();
  TheISA::PCState pc(funcStartVAddr);
  auto fetchPC = funcStartVAddr;
  // Feed in the first line.
  MachInst machInst;
  assert(prox.tryReadBlob(fetchPC, &machInst, sizeof(machInst)) &&
         "Failed to read in next machine inst.");

  while (true) {
    /**
     * Although wierd, this is used to feed in data to the decoder,
     * even if it's from previous line.
     */
    EXEC_FUNC_DPRINTF("Feed in %#x %#x %#x %#x %#x %#x %#x %#x %#x.\n", fetchPC,
                      (machInst >> 0) & 0xff, (machInst >> 8) & 0xff,
                      (machInst >> 16) & 0xff, (machInst >> 24) & 0xff,
                      (machInst >> 32) & 0xff, (machInst >> 40) & 0xff,
                      (machInst >> 48) & 0xff, (machInst >> 56) & 0xff);
    decoder.moreBytes(pc, fetchPC, machInst);
    // Read in the next machInst.
    if (decoder.needMoreBytes()) {
      fetchPC += sizeof(machInst);
      assert(prox.tryReadBlob(fetchPC, &machInst, sizeof(machInst)) &&
             "Failed to read in next machine inst.");
    }
    if (!decoder.instReady()) {
      EXEC_FUNC_DPRINTF("Feed in %#x %#x %#x %#x %#x %#x %#x %#x %#x.\n",
                        fetchPC, (machInst >> 0) & 0xff, (machInst >> 8) & 0xff,
                        (machInst >> 16) & 0xff, (machInst >> 24) & 0xff,
                        (machInst >> 32) & 0xff, (machInst >> 40) & 0xff,
                        (machInst >> 48) & 0xff, (machInst >> 56) & 0xff);
      decoder.moreBytes(pc, fetchPC, machInst);
    }
    assert(decoder.instReady() && "Decoder should have the inst ready.");
    auto staticInst = decoder.decode(pc);
    assert(staticInst && "Failed to decode inst.");

    // We assume there is no branch.
    EXEC_FUNC_DPRINTF("Decode MacroInst %s.\n",
                      staticInst->disassemble(pc.pc()).c_str());

    auto macroop = dynamic_cast<MacroopBase *>(staticInst.get());
    if (staticInst->getName() == "ret") {
      break;
    }

    assert(!staticInst->isControl() &&
           "No control instruction allowed in address function.");
    auto numMicroops = macroop->getNumMicroops();
    for (auto upc = 0; upc < numMicroops; ++upc) {
      auto microop = staticInst->fetchMicroop(upc);
      EXEC_FUNC_DPRINTF("  Decode MicroInst %s.\n",
                        microop->disassemble(pc.pc()).c_str());
      this->instructions.push_back(microop);
      this->pcs.push_back(pc);
    }

    // Advance to the next pc.
    pc.advance();
    EXEC_FUNC_DPRINTF("Next pc %#x.\n", pc.pc());
  }
  EXEC_FUNC_DPRINTF("Decode done.\n", pc.pc());
}

uint64_t ExecFunc::invoke(const std::vector<uint64_t> &params) {
  /**
   * We are assuming C calling convention.
   * Registers are passed in as $rdi, $rsi, $rdx, $rcx, $r8, $r9.
   * The exec function should never use stack.
   */
  assert(params.size() <= 6 && "Too many arguments for exec function.");
  assert(params.size() == this->func.args_size() &&
         "Mismatch with number of args.");
  execFuncXC.clear();

  const RegId intRegParams[6] = {
      RegId(RegClass::IntRegClass, IntRegIndex::INTREG_RDI),
      RegId(RegClass::IntRegClass, IntRegIndex::INTREG_RSI),
      RegId(RegClass::IntRegClass, IntRegIndex::INTREG_RDX),
      RegId(RegClass::IntRegClass, IntRegIndex::INTREG_RCX),
      RegId(RegClass::IntRegClass, IntRegIndex::INTREG_R8),
      RegId(RegClass::IntRegClass, IntRegIndex::INTREG_R9),
  };
  const RegId floatRegParams[8] = {
      RegId(RegClass::FloatRegClass, FloatRegIndex::FLOATREG_XMM0_0),
      RegId(RegClass::FloatRegClass, FloatRegIndex::FLOATREG_XMM1_0),
      RegId(RegClass::FloatRegClass, FloatRegIndex::FLOATREG_XMM2_0),
      RegId(RegClass::FloatRegClass, FloatRegIndex::FLOATREG_XMM3_0),
      RegId(RegClass::FloatRegClass, FloatRegIndex::FLOATREG_XMM4_0),
      RegId(RegClass::FloatRegClass, FloatRegIndex::FLOATREG_XMM5_0),
      RegId(RegClass::FloatRegClass, FloatRegIndex::FLOATREG_XMM6_0),
      RegId(RegClass::FloatRegClass, FloatRegIndex::FLOATREG_XMM7_0),
  };
  EXEC_FUNC_DPRINTF("Set up calling convention.\n");
  int intParamIdx = 0;
  int floatParamIdx = 0;
  for (auto idx = 0; idx < params.size(); ++idx) {
    auto param = params.at(idx);
    if (this->func.args(idx).is_float()) {
      const auto &reg = floatRegParams[floatParamIdx];
      floatParamIdx++;
      execFuncXC.setFloatRegOperand(reg, param);
      EXEC_FUNC_DPRINTF("Arg %d Reg %s %llu.\n", idx, reg, param);
    } else {
      const auto &reg = intRegParams[intParamIdx];
      intParamIdx++;
      execFuncXC.setIntRegOperand(reg, param);
      EXEC_FUNC_DPRINTF("Arg %d Reg %s %llu.\n", idx, reg, param);
    }
  }

  // Set up the virt proxy.
  execFuncXC.setVirtProxy(&this->tc->getVirtProxy());

  for (auto idx = 0; idx < this->instructions.size(); ++idx) {
    auto &staticInst = this->instructions.at(idx);
    auto &pc = this->pcs.at(idx);
    EXEC_FUNC_DPRINTF("Set PCState %s.\n", pc);
    execFuncXC.pcState(pc);
    staticInst->execute(&execFuncXC, nullptr /* traceData. */);
  }

  if (this->func.is_float()) {
    // We expect the float result in xmm0.
    const RegId xmm0(RegClass::FloatRegClass, FloatRegIndex::FLOATREG_XMM0_0);
    auto retValue = execFuncXC.readFloatRegOperand(xmm0);
    EXEC_FUNC_DPRINTF("Ret %#x.\n", retValue);
    return retValue;
  } else {
    // We expect the int result in rax.
    const RegId rax(RegClass::IntRegClass, IntRegIndex::INTREG_RAX);
    auto retValue = execFuncXC.readIntRegOperand(rax);
    EXEC_FUNC_DPRINTF("Ret %#x.\n", retValue);
    return retValue;
  }
}
} // namespace X86ISA