#include "arch/x86/insts/microavxop.hh"

#include <string>

#include "arch/x86/regs/misc.hh"
#include "cpu/exec_context.hh"

namespace X86ISA {
std::string
AVXOpBase::generateDisassembly(Addr pc,
                               const ::Loader::SymbolTable *symtab) const {
  std::stringstream response;

  printMnemonic(response, instMnem, mnemonic);
  printDestReg(response, 0, destSize);
  if (this->srcType == SrcType::Non) {
    return response.str();
  }
  response << ", ";
  printSrcReg(response, 0, srcSize);
  switch (this->srcType) {
  case RegReg: {
    response << ", ";
    printSrcReg(response, 1, srcSize);
    break;
  }
  case RegImm: {
    ccprintf(response, ", %#x", imm8);
    break;
  }
  case RegRegImm: {
    response << ", ";
    printSrcReg(response, 1, srcSize);
    ccprintf(response, ", %#x", imm8);
    break;
  }
  case RegRegReg: {
    response << ", ";
    printSrcReg(response, 1, srcSize);
    response << ", ";
    printSrcReg(response, 2, srcSize);
    break;
  }
  default:
    break;
  }
  return response.str();
}

AVXOpBase::FloatInt AVXOpBase::calcPackedBinaryOp(FloatInt src1, FloatInt src2,
                                                  BinaryOp op) const {
  FloatInt dest;
  if (this->srcSize == 4) {
    // 2 float.
    switch (op) {
    default:
      assert(false && "Invalid op type.");
    case BinaryOp::FloatAdd:
      dest.f.f1 = src1.f.f1 + src2.f.f1;
      dest.f.f2 = src1.f.f2 + src2.f.f2;
      break;
    case BinaryOp::FloatSub:
      dest.f.f1 = src1.f.f1 - src2.f.f1;
      dest.f.f2 = src1.f.f2 - src2.f.f2;
      break;
    case BinaryOp::FloatMul:
      dest.f.f1 = src1.f.f1 * src2.f.f1;
      dest.f.f2 = src1.f.f2 * src2.f.f2;
      break;
    case BinaryOp::FloatDiv:
      dest.f.f1 = src1.f.f1 / src2.f.f1;
      dest.f.f2 = src1.f.f2 / src2.f.f2;
      break;
    case BinaryOp::IntAdd:
      dest.si.i1 = src1.si.i1 + src2.si.i1;
      dest.si.i2 = src1.si.i2 + src2.si.i2;
      break;
    case BinaryOp::IntSub:
      dest.si.i1 = src1.si.i1 - src2.si.i1;
      dest.si.i2 = src1.si.i2 - src2.si.i2;
      break;
    case BinaryOp::IntAnd:
      dest.si.i1 = src1.si.i1 & src2.si.i1;
      dest.si.i2 = src1.si.i2 & src2.si.i2;
      break;
    case BinaryOp::IntCmpEq:
      dest.si.i1 = (src1.si.i1 == src2.si.i1) ? 0xFFFF : 0x0;
      dest.si.i2 = (src1.si.i2 == src2.si.i2) ? 0xFFFF : 0x0;
      break;
    case BinaryOp::UIntMul:
      dest.ui.i1 = src1.ui.i1 * src2.ui.i1;
      dest.ui.i2 = src1.ui.i2 * src2.ui.i2;
      break;
    case BinaryOp::SIntMin:
      dest.si.i1 = std::min(src1.si.i1, src2.si.i1);
      dest.si.i2 = std::min(src1.si.i2, src2.si.i2);
      break;
    }
  } else {
    // 1 double;
    switch (op) {
    default:
      assert(false && "Invalid op type.");
    case BinaryOp::FloatAdd:
      dest.d = src1.d + src2.d;
      break;
    case BinaryOp::FloatSub:
      dest.d = src1.d - src2.d;
      break;
    case BinaryOp::FloatMul:
      dest.d = src1.d * src2.d;
      break;
    case BinaryOp::FloatDiv:
      dest.d = src1.d / src2.d;
      break;
    case BinaryOp::IntAdd:
      dest.sl = src1.sl + src2.sl;
      break;
    case BinaryOp::IntSub:
      dest.sl = src1.sl - src2.sl;
      break;
    case BinaryOp::IntAnd:
      dest.sl = src1.sl & src2.sl;
      break;
    case BinaryOp::IntCmpEq:
      dest.sl = (src1.sl == src2.sl) ? 0xFFFFFFFF : 0x0;
      break;
    case BinaryOp::UIntMul:
      dest.ul = src1.ul * src2.ul;
      break;
    case BinaryOp::SIntMin:
      dest.sl = std::min(src1.sl, src2.sl);
      break;
    }
  }
  return dest;
}

void AVXOpBase::doPackedBinaryOp(ExecContext *xc, BinaryOp op) const {
  auto vRegs = destVL / sizeof(uint64_t);
  FloatInt src1;
  FloatInt src2;
  for (int i = 0; i < vRegs; i++) {
    src1.ul = xc->readFloatRegOperandBits(this, i * 2 + 0);
    src2.ul = xc->readFloatRegOperandBits(this, i * 2 + 1);
    auto dest = this->calcPackedBinaryOp(src1, src2, op);
    xc->setFloatRegOperandBits(this, i, dest.ul);
  }
}

void AVXOpBase::doFusedPackedBinaryOp(ExecContext *xc, BinaryOp op1,
                                      BinaryOp op2) const {
  auto vRegs = destVL / sizeof(uint64_t);
  FloatInt src1;
  FloatInt src2;
  FloatInt src3;
  for (int i = 0; i < vRegs; i++) {
    src1.ul = xc->readFloatRegOperandBits(this, i * 3 + 0);
    src2.ul = xc->readFloatRegOperandBits(this, i * 3 + 1);
    src3.ul = xc->readFloatRegOperandBits(this, i * 3 + 2);
    auto tmp = this->calcPackedBinaryOp(src1, src2, op1);
    auto dest = this->calcPackedBinaryOp(tmp, src3, op2);
    xc->setFloatRegOperandBits(this, i, dest.ul);
  }
}

void AVXOpBase::doPackOp(ExecContext *xc, BinaryOp op) const {
  auto vRegs = destVL / sizeof(uint64_t);
  switch (op) {
  default:
    panic("Unsupported pack op %d.", op);
  case BinaryOp::SIntToUIntPack: {
    for (int i = 0; i < vRegs; ++i) {
      FloatInt src1;
      FloatInt src2;
      int srcIdx = i - (i % 2);
      if ((i % 2) == 0) {
        // Take 128 bit from src1.
        src1.ul = xc->readFloatRegOperandBits(this, (srcIdx + 0) * 2 + 0);
        src2.ul = xc->readFloatRegOperandBits(this, (srcIdx + 1) * 2 + 0);
      } else {
        // Take 128 bit from src2.
        src1.ul = xc->readFloatRegOperandBits(this, (srcIdx + 0) * 2 + 1);
        src2.ul = xc->readFloatRegOperandBits(this, (srcIdx + 1) * 2 + 1);
      }
      FloatInt dest;
      if (this->srcSize == 4) {
        // Pack int32_t -> uint16_t.
#define SignedToUnsignedSaturate(v)                                            \
  v > 0xFFFF ? 0xFFFF : (v < 0 ? 0 : (v & 0xFFFF))
        dest.us.i1 = SignedToUnsignedSaturate(src1.si.i1);
        dest.us.i2 = SignedToUnsignedSaturate(src1.si.i2);
        dest.us.i3 = SignedToUnsignedSaturate(src2.si.i1);
        dest.us.i4 = SignedToUnsignedSaturate(src2.si.i2);
#undef SignedToUnsignedSaturate
      } else if (this->srcSize == 2) {
        // Pack int16_t -> uint8_t.
#define SignedToUnsignedSaturate(v) v > 0xFF ? 0xFF : (v < 0 ? 0 : (v & 0xFF))
        dest.uc.i1 = SignedToUnsignedSaturate(src1.ss.i1);
        dest.uc.i2 = SignedToUnsignedSaturate(src1.ss.i2);
        dest.uc.i3 = SignedToUnsignedSaturate(src1.ss.i3);
        dest.uc.i4 = SignedToUnsignedSaturate(src1.ss.i4);
        dest.uc.i5 = SignedToUnsignedSaturate(src2.ss.i1);
        dest.uc.i6 = SignedToUnsignedSaturate(src2.ss.i2);
        dest.uc.i7 = SignedToUnsignedSaturate(src2.ss.i3);
        dest.uc.i8 = SignedToUnsignedSaturate(src2.ss.i4);
#undef SignedToUnsignedSaturate
      } else {
        panic("Unsupported size for pack %d.", this->srcSize);
      }
      xc->setFloatRegOperandBits(this, i, dest.ul);
    }
    break;
  }
  }
}

void AVXOpBase::doExtract(ExecContext *xc) const {
  FloatInt result;
  result.ul = 0;
  auto select = imm8;
  if (srcSize == 1) {
    FloatInt src;
    if ((select >> 3) & 1) {
      src.ul = xc->readFloatRegOperandBits(this, 0);
    } else {
      src.ul = xc->readFloatRegOperandBits(this, 1);
    }
    // Extract the byte.
    result.uc.i1 = src.uc_array[select & 0x7];
  }
  // hack("%s.\n", this->generateDisassembly(0x0, nullptr));
  // hack("Extract %lu -> %s.\n", result.ul, this->destRegIdx(0));
  xc->setIntRegOperand(this, 0, result.ul);
}

} // namespace X86ISA
