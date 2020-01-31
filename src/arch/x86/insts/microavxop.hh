#ifndef __ARCH_X86_INSTS_MICROAVXOP_HH__
#define __ARCH_X86_INSTS_MICROAVXOP_HH__

#include "arch/x86/insts/microop.hh"

namespace X86ISA {

class AVXOpBase : public X86MicroopBase {

public:
  enum SrcType {
    Non = 0,
    Reg = 1,
    RegReg = 2,
    RegImm = 3,
    RegRegImm = 4,
  };

protected:
  const SrcType srcType;
  const RegIndex dest;
  const RegIndex src1;
  const RegIndex src2;
  const uint8_t destSize;
  const uint8_t destVL;
  const uint8_t srcSize;
  const uint8_t srcVL;
  const uint8_t imm8;
  const uint8_t ext;

  // Constructor
  AVXOpBase(ExtMachInst _machInst, const char *_mnem, const char *_instMnem,
            uint64_t _setFlags, OpClass _opClass, SrcType _srcType,
            InstRegIndex _dest, InstRegIndex _src1, InstRegIndex _src2,
            uint8_t _destSize, uint8_t _destVL, uint8_t _srcSize,
            uint8_t _srcVL, uint8_t _imm8, uint8_t _ext)
      : X86MicroopBase(_machInst, _mnem, _instMnem, _setFlags, _opClass),
        srcType(_srcType), dest(_dest.index()), src1(_src1.index()),
        src2(_src2.index()), destSize(_destSize), destVL(_destVL),
        srcSize(_srcSize), srcVL(_srcVL), imm8(_imm8), ext(_ext) {
    assert((destVL % sizeof(uint64_t) == 0) && "Invalid destVL.\n");
    assert((srcVL % sizeof(uint64_t) == 0) && "Invalid srcVL.\n");
  }

  std::string generateDisassembly(Addr pc, const SymbolTable *symtab) const;
};

} // namespace X86ISA

#endif //__ARCH_X86_INSTS_MICROAVXOP_HH__
