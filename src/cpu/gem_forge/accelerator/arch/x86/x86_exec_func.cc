#include "x86_exec_func.hh"

#include "../exec_func_context.hh"
#include "arch/x86/decoder.hh"
#include "arch/x86/insts/macroop.hh"
#include "arch/x86/regs/float.hh"
#include "base/loader/object_file.hh"
#include "base/loader/symtab.hh"
#include "cpu/exec_context.hh"
#include "sim/process.hh"

#include "cpu/gem_forge/accelerator/arch/gem_forge_isa_handler.hh"
#include "cpu/gem_forge/gem_forge_utils.hh"

#include "debug/ExecFunc.hh"

#define EXEC_FUNC_DPRINTF(format, args...)                                     \
  DPRINTF(ExecFunc, "[%s]: " format, this->func.name().c_str(), ##args)
#define EXEC_FUNC_PANIC(format, args...)                                       \
  panic("[%s]: " format, this->func.name().c_str(), ##args)

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
    if (pc.pc() >= fetchPC + sizeof(machInst)) {
      // Somehow we just happen to consumed all of machInst.
      fetchPC += sizeof(machInst);
      assert(prox.tryReadBlob(fetchPC, &machInst, sizeof(machInst)) &&
             "Failed to read in next machine inst.");
    }
  }
  EXEC_FUNC_DPRINTF("Decode done. Final PC %s.\n", pc);

  this->estimateLatency();

  // Check if I am pure integer type.
  this->isPureInteger = true;
  for (const auto &arg : this->func.args()) {
    if (arg.type() != DataType::INTEGER) {
      this->isPureInteger = false;
    }
    if (arg.type() == DataType::VECTOR_128 ||
        arg.type() == DataType::VECTOR_256 ||
        arg.type() == DataType::VECTOR_512) {
      if (this->getNumInstructions() > 0) {
        this->isSIMD = true;
      }
    }
  }
  if (this->func.type() != DataType::INTEGER) {
    this->isPureInteger = false;
  }
  if (this->func.type() == DataType::VECTOR_128 ||
      this->func.type() == DataType::VECTOR_256 ||
      this->func.type() == DataType::VECTOR_512) {
    if (this->getNumInstructions() > 0) {
      this->isSIMD = true;
    }
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
  case DataType::VOID:
    numRegs = 0;
    break;
  default:
    panic("Invalid data type.");
  }
  return numRegs;
}

std::string ExecFunc::RegisterValue::print(const DataType &type) const {
  if (type == DataType::INTEGER) {
    return csprintf("%#x", this->front());
  } else if (type == DataType::FLOAT) {
    return csprintf("%f", static_cast<float>(this->front()));
  } else if (type == DataType::DOUBLE) {
    return csprintf("%f", static_cast<double>(this->front()));
  } else {
    auto numRegs = ExecFunc::translateToNumRegs(type);
    return GemForgeUtils::dataToString(this->uint8Ptr(), numRegs * 8);
  }
}

std::string ExecFunc::RegisterValue::print() const {
  return GemForgeUtils::dataToString(this->uint8Ptr(), sizeof(*this));
}

ExecFunc::RegisterValue
ExecFunc::invoke(const std::vector<RegisterValue> &params,
                 GemForgeISAHandler *isaHandler, InstSeqNum startSeqNum) {
  /**
   * We are assuming C calling convention.
   * Registers are passed in as $rdi, $rsi, $rdx, $rcx, $r8, $r9.
   * The exec function should never use stack.
   */
  if (params.size() != this->func.args_size()) {
    panic("Invoke %s: Mismatch in # args, given %d, expected %d.\n",
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
      assert(intParamIdx < 6 && "Too many int arguments for exec function.");
      const auto &reg = intRegParams[intParamIdx];
      intParamIdx++;
      execFuncXC.setIntRegOperand(reg, param.front());
      EXEC_FUNC_DPRINTF("Arg %d Reg %s %s.\n", idx, reg, param.print(type));
    } else {
      assert(floatParamIdx < 8 &&
             "Too many float arguments for exec function.");
      auto numRegs = this->translateToNumRegs(type);
      const auto &baseReg = floatRegParams[floatParamIdx];
      floatParamIdx++;
      for (int i = 0; i < numRegs; ++i) {
        RegId reg = RegId(RegClass::FloatRegClass, baseReg.index() + i);
        execFuncXC.setFloatRegOperand(reg, param.at(i));
        EXEC_FUNC_DPRINTF("Arg %d Reg %s %s.\n", idx, reg, param.print(type));
      }
    }
  }

  // Set up the virt proxy.
  execFuncXC.setVirtProxy(&this->tc->getVirtProxy());

  for (auto idx = 0; idx < this->instructions.size(); ++idx) {
    auto &staticInst = this->instructions.at(idx);
    auto &pc = this->pcs.at(idx);
    EXEC_FUNC_DPRINTF("Set PCState %s: %s.\n", pc,
                      staticInst->disassemble(pc.pc()));
    execFuncXC.pcState(pc);
    staticInst->execute(&execFuncXC, nullptr /* traceData. */);

    /**
     * Handle GemForge instructions. For now this is used for
     * NestStreamConfigureFunc.
     */
    if (staticInst->isGemForge()) {
      GemForgeDynInstInfo dynInfo(startSeqNum + idx, pc, staticInst.get(),
                                  this->tc);
      assert(isaHandler->canDispatch(dynInfo) && "Cannot dispatch.");
      GemForgeLSQCallbackList lsqCallbacks;
      isaHandler->dispatch(dynInfo, lsqCallbacks);
      assert(!lsqCallbacks.front() && "Cannot handle extra LSQ callbacks.");
      assert(isaHandler->canExecute(dynInfo) && "Cannot execute.");
      isaHandler->execute(dynInfo, execFuncXC);
      assert(isaHandler->canCommit(dynInfo) && "Cannot commit.");
      isaHandler->commit(dynInfo);
    }
  }

  auto retType = this->func.type();
  RegisterValue retValue;
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
  EXEC_FUNC_DPRINTF("Ret Type %s.\n", ::LLVM::TDG::DataType_Name(retType),
                    retValue.print(retType));
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

void ExecFunc::estimateLatency() {
  this->estimatedLatency = Cycles(0);
  for (auto &staticInst : this->instructions) {
    int lat = 0;
    switch (staticInst->opClass()) {
    default:
      lat = 0;
      break;
    case Enums::OpClass::No_OpClass:
      lat = 0;
      break;
    case Enums::OpClass::IntAlu:
      lat = 1;
      break;
    case Enums::OpClass::IntMult:
      lat = 3;
      break;
    case Enums::OpClass::IntDiv:
      lat = 12;
      break;
    case Enums::OpClass::FloatAdd:
    case Enums::OpClass::FloatCmp:
    case Enums::OpClass::FloatCvt:
    case Enums::OpClass::FloatMisc:
      lat = 2;
      break;
    case Enums::OpClass::FloatMult:
      lat = 1;
      break;
    case Enums::OpClass::FloatMultAcc:
      lat = 3;
      break;
    case Enums::OpClass::FloatDiv:
      lat = 12;
      break;
    case Enums::OpClass::FloatSqrt:
      lat = 20;
      break;
    case Enums::OpClass::SimdAdd:
    case Enums::OpClass::SimdAddAcc:
    case Enums::OpClass::SimdAlu:
    case Enums::OpClass::SimdCmp:
    case Enums::OpClass::SimdCvt:
    case Enums::OpClass::SimdMisc:
    case Enums::OpClass::SimdShift:
      lat = 1;
      break;
    case Enums::OpClass::SimdMult:
      lat = 3;
      break;
    case Enums::OpClass::SimdMultAcc:
      lat = 4;
      break;
    case Enums::OpClass::SimdShiftAcc:
      lat = 2;
      break;
    case Enums::OpClass::SimdDiv:
      lat = 12;
      break;
    case Enums::OpClass::SimdSqrt:
      lat = 20;
      break;
    case Enums::OpClass::SimdFloatAdd:
    case Enums::OpClass::SimdFloatAlu:
    case Enums::OpClass::SimdFloatCmp:
    case Enums::OpClass::SimdFloatCvt:
    case Enums::OpClass::SimdFloatMisc:
      lat = 2;
      break;
    case Enums::OpClass::SimdFloatDiv:
      lat = 12;
      break;
    case Enums::OpClass::SimdFloatMult:
      lat = 3;
      break;
    case Enums::OpClass::SimdFloatMultAcc:
      lat = 4;
      break;
    case Enums::OpClass::SimdFloatSqrt:
      lat = 20;
      break;
    case Enums::OpClass::SimdReduceAdd:
      lat = 1;
      break;
    case Enums::OpClass::SimdReduceAlu:
    case Enums::OpClass::SimdReduceCmp:
    case Enums::OpClass::SimdFloatReduceAdd:
    case Enums::OpClass::SimdFloatReduceCmp:
      lat = 2;
      break;
    case Enums::OpClass::SimdAes:
    case Enums::OpClass::SimdAesMix:
    case Enums::OpClass::SimdSha1Hash:
    case Enums::OpClass::SimdSha1Hash2:
    case Enums::OpClass::SimdSha256Hash:
    case Enums::OpClass::SimdSha256Hash2:
    case Enums::OpClass::SimdShaSigma2:
    case Enums::OpClass::SimdShaSigma3:
      lat = 20;
      break;
    case Enums::OpClass::SimdPredAlu:
      lat = 1;
      break;
    case Enums::OpClass::MemRead:
    case Enums::OpClass::MemWrite:
    case Enums::OpClass::FloatMemRead:
    case Enums::OpClass::FloatMemWrite:
      // MemRead is only used to read the immediate number.
      lat = 0;
      break;
    case Enums::OpClass::IprAccess:
    case Enums::OpClass::InstPrefetch:
    case Enums::OpClass::Accelerator:
      lat = 1;
      break;
    }
    this->estimatedLatency += Cycles(lat);
  }
}
} // namespace X86ISA

std::ostream &operator<<(std::ostream &os,
                         const X86ISA::ExecFunc::RegisterValue &value) {
  os << value.print();
  return os;
}