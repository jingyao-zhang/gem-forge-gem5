#include "x86_exec_func.hh"

#include "../exec_func_context.hh"
#include "arch/x86/decoder.hh"
#include "arch/x86/insts/macroop.hh"
#include "arch/x86/regs/float.hh"
#include "base/loader/object_file.hh"
#include "base/loader/symtab.hh"
#include "cpu/exec_context.hh"
#include "cpu/gem_forge/gem_forge_utils.hh"
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
  ::Loader::SymbolTable table;
  obj->loadAllSymbols(&table);
  Addr funcStartVAddr;
  if (!table.findAddress(this->func.name(), funcStartVAddr)) {
    panic("Failed to resovle symbol: %s.", this->func.name());
  }

  auto dsEffBase = tc->readMiscRegNoEffect(MiscRegIndex::MISCREG_DS_EFF_BASE);
  auto csEffBase = tc->readMiscRegNoEffect(MiscRegIndex::MISCREG_CS_EFF_BASE);
  assert(dsEffBase == 0 && "We cannot handle this DS.\n");
  assert(csEffBase == 0 && "We cannot handle this CS.\n");

  EXEC_FUNC_DPRINTF("======= Start decoding %s from %#x.\n", this->func.name(),
                    funcStartVAddr);

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

  // Check if I am pure integer type.
  this->isPureInteger = true;
  for (const auto &arg : this->func.args()) {
    if (arg.type() != DataType::INTEGER) {
      this->isPureInteger = false;
      return;
    }
  }
  if (this->func.type() != DataType::INTEGER) {
    this->isPureInteger = false;
  }
}

int ExecFunc::translateToNumRegs(const DataType &type) {
  auto numRegs = 1;
  switch (type) {
  case DataType::FLOAT:
  case DataType::DOUBLE:
    numRegs = 1;
    break;
  case DataType::VECTOR_128:
    numRegs = 2;
    break;
  case DataType::VECTOR_256:
    numRegs = 4;
    break;
  case DataType::VECTOR_512:
    numRegs = 8;
    break;
  default:
    panic("Invalid data type.");
  }
  return numRegs;
}

std::string ExecFunc::printRegisterValue(const RegisterValue &value,
                                         const DataType &type) {
  if (type == DataType::INTEGER) {
    return csprintf("%#x", value.front());
  } else if (type == DataType::FLOAT) {
    return csprintf("%f", static_cast<float>(value.front()));
  } else if (type == DataType::DOUBLE) {
    return csprintf("%f", static_cast<double>(value.front()));
  } else {
    auto numRegs = ExecFunc::translateToNumRegs(type);
    return GemForgeUtils::dataToString(
        reinterpret_cast<const uint8_t *>(value.data()), numRegs * 8);
  }
}

ExecFunc::RegisterValue
ExecFunc::invoke(const std::vector<RegisterValue> &params) {
  /**
   * We are assuming C calling convention.
   * Registers are passed in as $rdi, $rsi, $rdx, $rcx, $r8, $r9.
   * The exec function should never use stack.
   */
  assert(params.size() <= 6 && "Too many arguments for exec function.");
  if (params.size() != this->func.args_size()) {
    panic("Invoke %s: Mismatch in # args, given %d, expected.\n",
          this->func.name(), params.size(), this->func.args_size());
  }
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
    auto type = this->func.args(idx).type();
    if (type == ::LLVM::TDG::DataType::INTEGER) {
      const auto &reg = intRegParams[intParamIdx];
      intParamIdx++;
      execFuncXC.setIntRegOperand(reg, param.front());
      EXEC_FUNC_DPRINTF("Arg %d Reg %s %s.\n", idx, reg,
                        printRegisterValue(param, type));
    } else {
      auto numRegs = this->translateToNumRegs(type);
      const auto &baseReg = floatRegParams[floatParamIdx];
      floatParamIdx++;
      for (int i = 0; i < numRegs; ++i) {
        RegId reg = RegId(RegClass::FloatRegClass, baseReg.index() + i);
        execFuncXC.setFloatRegOperand(reg, param.at(i));
        EXEC_FUNC_DPRINTF("Arg %d Reg %s %s.\n", idx, reg,
                          printRegisterValue(param, type));
      }
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

  auto retType = this->func.type();
  RegisterValue retValue{0};
  if (retType == ::LLVM::TDG::DataType::INTEGER) {
    // We expect the int result in rax.
    const RegId rax(RegClass::IntRegClass, IntRegIndex::INTREG_RAX);
    retValue.front() = execFuncXC.readIntRegOperand(rax);
  } else {
    // We expect the float result in xmm0.
    auto numRegs = this->translateToNumRegs(retType);
    for (int i = 0; i < numRegs; ++i) {
      RegId reg =
          RegId(RegClass::FloatRegClass, FloatRegIndex::FLOATREG_XMM0_0 + i);
      retValue.at(i) = execFuncXC.readFloatRegOperand(reg);
    }
  }
  EXEC_FUNC_DPRINTF("Ret %s.\n", printRegisterValue(retValue, retType));
  return retValue;
}

Addr ExecFunc::invoke(const std::vector<Addr> &params) {
  /**
   * We are assuming C calling convention.
   * Registers are passed in as $rdi, $rsi, $rdx, $rcx, $r8, $r9.
   * The exec function should never use stack.
   */
  assert(params.size() <= 6 && "Too many arguments for exec function.");
  if (params.size() != this->func.args_size()) {
    panic("Invoke %s: Mismatch in # args, given %d, expected.\n",
          this->func.name(), params.size(), this->func.args_size());
  }
  if (!this->isPureInteger) {
    panic("Invoke %s: Should be pure integer.\n", this->func.name());
  }
  execFuncXC.clear();

  const RegId intRegParams[6] = {
      RegId(RegClass::IntRegClass, IntRegIndex::INTREG_RDI),
      RegId(RegClass::IntRegClass, IntRegIndex::INTREG_RSI),
      RegId(RegClass::IntRegClass, IntRegIndex::INTREG_RDX),
      RegId(RegClass::IntRegClass, IntRegIndex::INTREG_RCX),
      RegId(RegClass::IntRegClass, IntRegIndex::INTREG_R8),
      RegId(RegClass::IntRegClass, IntRegIndex::INTREG_R9),
  };
  EXEC_FUNC_DPRINTF("Set up calling convention.\n");
  for (auto idx = 0; idx < params.size(); ++idx) {
    auto param = params.at(idx);
    const auto &reg = intRegParams[idx];
    execFuncXC.setIntRegOperand(reg, param);
    EXEC_FUNC_DPRINTF("Arg %d Reg %s %#x.\n", idx, reg, param);
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

  // We expect the int result in rax.
  const RegId rax(RegClass::IntRegClass, IntRegIndex::INTREG_RAX);
  Addr retValue = execFuncXC.readIntRegOperand(rax);
  EXEC_FUNC_DPRINTF("Ret %#x.\n", retValue);
  return retValue;
}
} // namespace X86ISA