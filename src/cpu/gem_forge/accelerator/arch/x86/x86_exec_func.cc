#include "x86_exec_func.hh"

#include "../exec_func_context.hh"
#include "arch/x86/decoder.hh"
#include "arch/x86/insts/macroop.hh"
#include "arch/x86/insts/microop.hh"
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

namespace gem5 {

namespace {

/**
 * Since gem5 is single thread, and all the address computation is not
 * overlapped, we use a static global context.
 */
static ExecFuncContext execFuncXC;

} // namespace

namespace X86ISA {

namespace {
const RegId intRegParams[6] = {
    int_reg::Rdi, int_reg::Rsi, int_reg::Rdx,
    int_reg::Rcx, int_reg::R8,  int_reg::R9,
};
const RegId floatRegParams[8] = {
    float_reg::xmmIdx(0, 0), float_reg::xmmIdx(1, 0), float_reg::xmmIdx(2, 0),
    float_reg::xmmIdx(3, 0), float_reg::xmmIdx(4, 0), float_reg::xmmIdx(5, 0),
    float_reg::xmmIdx(6, 0), float_reg::xmmIdx(7, 0),
};
} // namespace

ExecFunc::ExecFunc(ThreadContext *_tc, const ::LLVM::TDG::ExecFuncInfo &_func)
    : tc(_tc), func(_func) {

  execFuncXC.init(tc->getIsaPtr());

  auto p = tc->getProcessPtr();
  auto obj = p->objFile;
  Addr funcStartVAddr;
  if (!obj->symtab().findAddress(this->func.name(), funcStartVAddr)) {
    panic("Failed to resovle symbol: %s.", this->func.name());
  }

  assert(tc->readMiscRegNoEffect(misc_reg::DsEffBase) == 0 &&
         "We cannot handle this DS.\n");
  assert(tc->readMiscRegNoEffect(misc_reg::CsEffBase) == 0 &&
         "We cannot handle this CS.\n");

  EXEC_FUNC_DPRINTF("======= Start decoding %s from %#x.\n", this->func.name(),
                    funcStartVAddr);

  auto &prox = this->tc->getVirtProxy();

  /**
   * Let's create a new decoder to avoid interfering the original one's state.
   */
  auto dec = dynamic_cast<TheISA::Decoder *>(this->tc->getDecoderPtr());
  TheISA::Decoder decoder(dec->params());
  decoder.takeOverFrom(dec);
  decoder.reset();
  TheISA::PCState pc(funcStartVAddr);
  auto fetchPC = funcStartVAddr;
  // Feed in the first line.
  MachInst &machInst = *static_cast<MachInst *>(decoder.moreBytesPtr());
  panic_if(!prox.tryReadBlob(fetchPC, &machInst, sizeof(MachInst)),
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
    decoder.moreBytes(pc, fetchPC);
    // Read in the next machInst.
    if (decoder.needMoreBytes()) {
      fetchPC += sizeof(machInst);
      panic_if(!prox.tryReadBlob(fetchPC, &machInst, sizeof(MachInst)),
               "Failed to read in next machine inst.");
    }
    if (!decoder.instReady()) {
      EXEC_FUNC_DPRINTF("Feed in %#x %#x %#x %#x %#x %#x %#x %#x %#x.\n",
                        fetchPC, (machInst >> 0) & 0xff, (machInst >> 8) & 0xff,
                        (machInst >> 16) & 0xff, (machInst >> 24) & 0xff,
                        (machInst >> 32) & 0xff, (machInst >> 40) & 0xff,
                        (machInst >> 48) & 0xff, (machInst >> 56) & 0xff);
      decoder.moreBytes(pc, fetchPC);
      if (!decoder.instReady()) {
        fetchPC += sizeof(machInst);
        panic_if(!prox.tryReadBlob(fetchPC, &machInst, sizeof(machInst)),
                 "Failed to read in next machine inst.");
        EXEC_FUNC_DPRINTF("Feed in %#x %#x %#x %#x %#x %#x %#x %#x %#x.\n",
                          fetchPC, (machInst >> 0) & 0xff,
                          (machInst >> 8) & 0xff, (machInst >> 16) & 0xff,
                          (machInst >> 24) & 0xff, (machInst >> 32) & 0xff,
                          (machInst >> 40) & 0xff, (machInst >> 48) & 0xff,
                          (machInst >> 56) & 0xff);
        decoder.moreBytes(pc, fetchPC);
      }
    }
    assert(decoder.instReady() && "Decoder should have the inst ready.");
    auto staticInst = decoder.decode(pc);
    assert(staticInst && "Failed to decode inst.");

    // We assume there is no branch.
    EXEC_FUNC_DPRINTF("Decode MacroInst %#x %s.\n", pc.pc(),
                      staticInst->disassemble(pc.pc()));

    auto macroop = dynamic_cast<MacroopBase *>(staticInst.get());
    if (staticInst->getName() == "ret") {
      break;
    }

    auto numMicroops = macroop->getNumMicroops();
    for (auto upc = 0; upc < numMicroops; ++upc) {
      auto microop = staticInst->fetchMicroop(upc);
      EXEC_FUNC_DPRINTF("  Decode MicroInst %s %s.\n",
                        enums::OpClassStrings[microop->opClass()],
                        microop->disassemble(pc.pc()));
      this->instructions.push_back(microop);
      this->pcs.push_back(pc);
    }

    // Advance to the next pc.
    pc.advance();
    EXEC_FUNC_DPRINTF("Next pc %#x.\n", pc.pc());
    if (pc.pc() >= fetchPC + sizeof(machInst)) {
      // Somehow we just happen to consumed all of machInst.
      fetchPC += sizeof(machInst);
      panic_if(!prox.tryReadBlob(fetchPC, &machInst, sizeof(machInst)),
               "Failed to read in next machine inst.");
    }
  }
  EXEC_FUNC_DPRINTF("Decode done. Final PC %s.\n", pc);

  for (const auto inst : this->instructions) {
    if (inst->getName() == "ssp_stream_ready") {
      break;
    }
    this->numInstsBeforeStreamConfig++;
  }

  this->estimateLatency();

  // Check if I am pure integer type.
  this->isPureInteger = true;
  for (const auto &arg : this->func.args()) {
    if (arg.type() != DataType::INTEGER && arg.type() != DataType::INT1) {
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
  if (this->func.type() != DataType::INTEGER &&
      this->func.type() != DataType::INT1) {
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
  if (type == DataType::INTEGER || type == DataType::INT1) {
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

  EXEC_FUNC_DPRINTF("Set up calling convention. Params %d.\n", params.size());
  int intParamIdx = 0;
  int floatParamIdx = 0;

  /**
   * Set up the stack parameters.
   */
  std::vector<RegVal> stackArgs;

  for (auto idx = 0; idx < params.size(); ++idx) {
    auto param = params.at(idx);
    auto type = this->func.args(idx).type();
    if (type == ::LLVM::TDG::DataType::INTEGER ||
        type == ::LLVM::TDG::DataType::INT1) {
      if (intParamIdx < 6) {
        const auto &reg = intRegParams[intParamIdx];
        execFuncXC.setRegOperand(reg, param.front());
        EXEC_FUNC_DPRINTF("Arg %d Reg %s %s.\n", idx, reg, param.print(type));
      } else {
        stackArgs.push_back(param.front());
        EXEC_FUNC_DPRINTF("Arg %d Stack %s.\n", idx, param.print(type));
      }
      intParamIdx++;
    } else {
      auto numRegs = this->translateToNumRegs(type);
      if (floatParamIdx < 8) {
        const auto &baseReg = floatRegParams[floatParamIdx];
        for (int i = 0; i < numRegs; ++i) {
          RegId reg = RegId(baseReg.regClass(), baseReg.index() + i);
          execFuncXC.setRegOperand(reg, param.at(i));
          EXEC_FUNC_DPRINTF("Arg %d Reg %s %s.\n", idx, reg, param.print(type));
        }
      } else {
        for (int i = 0; i < numRegs; ++i) {
          stackArgs.push_back(param.uint64(i));
          EXEC_FUNC_DPRINTF("Arg %d-%d Stack %s.\n", idx, i, param.print(type));
        }
      }
      floatParamIdx++;
    }
  }

  // We need to push arguments reversely.
  Addr curFakeStackVAddr = ExecFuncContext::FAKE_STACK_TOP_VADDR;
  for (int i = 0; i < stackArgs.size(); ++i) {
    const auto &arg = stackArgs[stackArgs.size() - 1 - i];
    curFakeStackVAddr -= 8;
    execFuncXC.storeFakeStack(curFakeStackVAddr, arg);
  }

  // Subtract the final fake return address from rsp.
  curFakeStackVAddr -= 8;

  auto RSP = int_reg::Rsp;
  execFuncXC.setRegOperand(RSP, curFakeStackVAddr);

  // Set up the virt proxy.
  auto &vproxy = this->tc->getVirtProxy();
  execFuncXC.setVirtProxy(&vproxy);

  // Support a simple stack. This is used for complex NestStreamConfigureFunc.
  std::vector<RegVal> stack;

  for (auto idx = 0; idx < this->instructions.size();) {
    auto &staticInst = this->instructions.at(idx);
    const auto &pc = this->pcs.at(idx);
    EXEC_FUNC_DPRINTF("Set PCState %s: %s.\n", pc,
                      staticInst->disassemble(pc.pc()));
    execFuncXC.pcState(pc);

    auto x86uop = dynamic_cast<X86ISA::X86MicroopBase *>(staticInst.get());
    auto instMnem = x86uop->getInstMnem();
    auto uopMnem = x86uop->getName();

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

    if (staticInst->isControl()) {
      const auto &newPC =
          dynamic_cast<const X86ISA::PCState &>(execFuncXC.pcState());
      EXEC_FUNC_DPRINTF("Ctrl Inst set PCState %s.\n", newPC);

      if (newPC.npc() != pc.npc()) {
        /**
         * We branched. Search for the target inst.
         */
        auto nextIdx = -1;
        for (int i = 0; i < this->instructions.size(); ++i) {
          auto nextPC = this->pcs.at(i);
          if (nextPC.pc() == newPC.npc()) {
            // Found it.
            nextIdx = i;
            break;
          }
        }

        if (nextIdx == -1) {
          EXEC_FUNC_PANIC("Failed to find JumpTarget %s.", newPC);
        }

        if (nextIdx <= idx) {
          EXEC_FUNC_PANIC("Backward Jump not supported.");
        }

        EXEC_FUNC_DPRINTF("Ctrl Inst Jump to Idx %d %s.\n", nextIdx, newPC);
        idx = nextIdx;
      } else {
        ++idx;
      }
    } else {
      ++idx;
    }
  }

  auto retType = this->func.type();
  RegisterValue retValue;
  if (retType == ::LLVM::TDG::DataType::INTEGER) {
    // We expect the int result in rax.
    auto rax = int_reg::Rax;
    retValue.front() = execFuncXC.getRegOperand(rax);
  } else if (retType == ::LLVM::TDG::DataType::INT1) {
    // We expect the int result in rax.
    auto rax = int_reg::Rax;
    retValue.front() = execFuncXC.getRegOperand(rax);
    retValue.front() &= 0x1;
  } else {
    // We expect the float result in xmm0.
    auto numRegs = this->translateToNumRegs(retType);
    for (int i = 0; i < numRegs; ++i) {
      auto reg = float_reg::xmmIdx(0, i);
      retValue.at(i) = execFuncXC.getRegOperand(reg);
    }
  }
  EXEC_FUNC_DPRINTF("Ret Type %s %s.\n", ::LLVM::TDG::DataType_Name(retType),
                    retValue.print(retType));
  return retValue;
}

std::vector<ExecFunc::RegisterValue>
ExecFunc::invoke_imvals(const std::vector<RegisterValue> &params,
                        GemForgeISAHandler *isaHandler,
                        InstSeqNum startSeqNum) {
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

  EXEC_FUNC_DPRINTF("Set up calling convention.\n");
  int intParamIdx = 0;
  int floatParamIdx = 0;
  for (auto idx = 0; idx < params.size(); ++idx) {
    auto param = params.at(idx);
    auto type = this->func.args(idx).type();
    if (type == ::LLVM::TDG::DataType::INTEGER ||
        type == ::LLVM::TDG::DataType::INT1) {
      if (intParamIdx == 6) {
        panic("Too many IntArgs on ExecFunc %s.", this->func.name());
      }
      const auto &reg = intRegParams[intParamIdx];
      intParamIdx++;
      execFuncXC.setRegOperand(reg, param.front());
      EXEC_FUNC_DPRINTF("Arg %d Reg %s %s.\n", idx, reg, param.print(type));
    } else {
      if (floatParamIdx == 8) {
        panic("Too many FloatArgs on ExecFunc %s.", this->func.name());
      }
      auto numRegs = this->translateToNumRegs(type);
      const auto &baseReg = floatRegParams[floatParamIdx];
      floatParamIdx++;
      for (int i = 0; i < numRegs; ++i) {
        RegId reg = RegId(baseReg.regClass(), baseReg.index() + i);
        execFuncXC.setRegOperand(reg, param.at(i));
        EXEC_FUNC_DPRINTF("Arg %d Reg %s %s.\n", idx, reg, param.print(type));
      }
    }
  }

  // Set up the virt proxy.
  auto &vproxy = this->tc->getVirtProxy();
  execFuncXC.setVirtProxy(&vproxy);

  std::vector<ExecFunc::RegisterValue> imvals;

  // Support a simple stack. This is used for complex NestStreamConfigureFunc.
  std::vector<RegVal> stack;

  for (auto idx = 0; idx < this->instructions.size(); ++idx) {
    auto &staticInst = this->instructions.at(idx);
    auto &pc = this->pcs.at(idx);
    EXEC_FUNC_DPRINTF("Set PCState %s: %s.\n", pc,
                      staticInst->disassemble(pc.pc()));
    execFuncXC.pcState(pc);

    auto x86uop = dynamic_cast<X86ISA::X86MicroopBase *>(staticInst.get());
    auto instMnem = x86uop->getInstMnem();
    auto uopMnem = x86uop->getName();

    if (instMnem == "PUSH_R") {
      if (uopMnem == "stis") {
        // Push into our small stack.
        // The 3rd argument is the data register.
        assert(staticInst->numSrcRegs() == 4);
        auto data = execFuncXC.getRegOperand(staticInst->srcRegIdx(2));
        stack.push_back(data);
        EXEC_FUNC_DPRINTF("Push %lu.\n", data);
      }
      // Ignore other microops.
      continue;
    }

    if (instMnem == "POP_R") {
      if (uopMnem == "mov") {
        // Pop from our small stack.
        // The 3rd argument is the data register.
        assert(staticInst->numDestRegs() == 1);
        auto data = stack.back();
        stack.pop_back();
        execFuncXC.setRegOperand(staticInst->destRegIdx(0), data);
        EXEC_FUNC_DPRINTF("Pop %lu.\n", data);
      }
      // Ignore other microops.
      continue;
    }

    staticInst->execute(&execFuncXC, nullptr /* traceData. */);

    /**
     * Zhengrong: Some instructions have CC flags.
     * And some have more than one dest regs.
     */
    assert(staticInst->numDestRegs() >= 1);
    imvals.emplace_back();
    auto reg = staticInst->destRegIdx(0);
    if (reg.classValue() == RegClassType::IntRegClass) {
      imvals.back().front() = execFuncXC.getRegOperand(reg);
    } else if (reg.classValue() == RegClassType::FloatRegClass) {
      // TODO: this should be the entire vector register
      imvals.back().at(0) = execFuncXC.getRegOperand(reg);
    } else if (reg.classValue() == RegClassType::CCRegClass) {
      imvals.back().at(0) = execFuncXC.getRegOperand(reg);
    } else {
      EXEC_FUNC_PANIC("Unsupported register type %s\n", reg);
    }

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
    auto rax = int_reg::Rax;
    retValue.front() = execFuncXC.getRegOperand(rax);
  } else if (retType == ::LLVM::TDG::DataType::INT1) {
    // We expect the int result in rax.
    auto rax = int_reg::Rax;
    retValue.front() = execFuncXC.getRegOperand(rax);
    retValue.front() &= 0x1;
  } else {
    // We expect the float result in xmm0.
    auto numRegs = this->translateToNumRegs(retType);
    for (int i = 0; i < numRegs; ++i) {
      RegId reg = float_reg::xmmIdx(0, i);
      retValue.at(i) = execFuncXC.getRegOperand(reg);
    }
  }
  EXEC_FUNC_DPRINTF("Ret Type %s.\n", ::LLVM::TDG::DataType_Name(retType),
                    retValue.print(retType));

  imvals.push_back(retValue);

  return imvals;
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

  EXEC_FUNC_DPRINTF("Set up calling convention.\n");
  for (auto idx = 0; idx < params.size(); ++idx) {
    auto param = params.at(idx);
    const auto &reg = intRegParams[idx];
    execFuncXC.setRegOperand(reg, param);
    EXEC_FUNC_DPRINTF("Arg %d Reg %s %#x.\n", idx, reg, param);
  }

  // Set up the virt proxy.
  auto &vproxy = this->tc->getVirtProxy();
  execFuncXC.setVirtProxy(&vproxy);

  for (auto idx = 0; idx < this->instructions.size(); ++idx) {
    auto &staticInst = this->instructions.at(idx);
    auto &pc = this->pcs.at(idx);
    EXEC_FUNC_DPRINTF("Set PCState %s.\n", pc);
    execFuncXC.pcState(pc);
    staticInst->execute(&execFuncXC, nullptr /* traceData. */);
  }

  // We expect the int result in rax.
  auto rax = int_reg::Rax;
  Addr retValue = execFuncXC.getRegOperand(rax);
  EXEC_FUNC_DPRINTF("Ret %#x.\n", retValue);
  return retValue;
}

Cycles ExecFunc::estimateOneInstLat(const StaticInstPtr &staticInst) const {

  int lat = 0;
  switch (staticInst->opClass()) {
  default:
    lat = 0;
    break;
  case enums::OpClass::No_OpClass:
    lat = 0;
    break;
  case enums::OpClass::IntAlu:
    lat = 1;
    break;
  case enums::OpClass::IntMult:
    lat = 3;
    break;
  case enums::OpClass::IntDiv:
    lat = 12;
    break;
  case enums::OpClass::FloatAdd:
  case enums::OpClass::FloatCmp:
  case enums::OpClass::FloatCvt:
  case enums::OpClass::FloatMisc:
    lat = 2;
    break;
  case enums::OpClass::FloatMult:
    lat = 1;
    break;
  case enums::OpClass::FloatMultAcc:
    lat = 3;
    break;
  case enums::OpClass::FloatDiv:
    lat = 12;
    break;
  case enums::OpClass::FloatSqrt:
    lat = 20;
    break;
  case enums::OpClass::SimdAdd:
  case enums::OpClass::SimdAddAcc:
  case enums::OpClass::SimdAlu:
  case enums::OpClass::SimdCmp:
  case enums::OpClass::SimdCvt:
  case enums::OpClass::SimdMisc:
  case enums::OpClass::SimdShift:
    lat = 1;
    break;
  case enums::OpClass::SimdMult:
    lat = 3;
    break;
  case enums::OpClass::SimdMultAcc:
    lat = 4;
    break;
  case enums::OpClass::SimdShiftAcc:
    lat = 2;
    break;
  case enums::OpClass::SimdDiv:
    lat = 12;
    break;
  case enums::OpClass::SimdSqrt:
    lat = 20;
    break;
  case enums::OpClass::SimdFloatAdd:
  case enums::OpClass::SimdFloatAlu:
  case enums::OpClass::SimdFloatCmp:
  case enums::OpClass::SimdFloatCvt:
  case enums::OpClass::SimdFloatMisc:
    lat = 2;
    break;
  case enums::OpClass::SimdFloatDiv:
    lat = 12;
    break;
  case enums::OpClass::SimdFloatMult:
    lat = 3;
    break;
  case enums::OpClass::SimdFloatMultAcc:
    lat = 4;
    break;
  case enums::OpClass::SimdFloatSqrt:
    lat = 20;
    break;
  case enums::OpClass::SimdReduceAdd:
    lat = 1;
    break;
  case enums::OpClass::SimdReduceAlu:
  case enums::OpClass::SimdReduceCmp:
  case enums::OpClass::SimdFloatReduceAdd:
  case enums::OpClass::SimdFloatReduceCmp:
    lat = 2;
    break;
  case enums::OpClass::SimdAes:
  case enums::OpClass::SimdAesMix:
  case enums::OpClass::SimdSha1Hash:
  case enums::OpClass::SimdSha1Hash2:
  case enums::OpClass::SimdSha256Hash:
  case enums::OpClass::SimdSha256Hash2:
  case enums::OpClass::SimdShaSigma2:
  case enums::OpClass::SimdShaSigma3:
    lat = 20;
    break;
  case enums::OpClass::SimdPredAlu:
    lat = 1;
    break;
  case enums::OpClass::MemRead:
  case enums::OpClass::MemWrite:
  case enums::OpClass::FloatMemRead:
  case enums::OpClass::FloatMemWrite:
    // MemRead is only used to read the immediate number.
    lat = 0;
    break;
  case enums::OpClass::IprAccess:
  case enums::OpClass::InstPrefetch:
  case enums::OpClass::Accelerator:
    lat = 1;
    break;
  }
  return Cycles(lat);
}

void ExecFunc::estimateLatency() {
  this->estimatedLatency = Cycles(0);
  for (auto &staticInst : this->instructions) {
    this->estimatedLatency += this->estimateOneInstLat(staticInst);
  }
}

std::ostream &operator<<(std::ostream &os,
                         const X86ISA::ExecFunc::RegisterValue &value) {
  os << value.print();
  return os;
}

} // namespace X86ISA
} // namespace gem5
