#ifndef __GEM_FORGE_EXEC_FUNC_CONTEXT_HH__
#define __GEM_FORGE_EXEC_FUNC_CONTEXT_HH__

#include "cpu/exec_context.hh"

#if THE_ISA == X86_ISA
// ! Jesus I break the isolation.
#include "arch/x86/regs/misc.hh"
#endif

/**
 * A taylored ExecContext that only provides integer register file,
 * for the address computation.
 */
class ExecFuncContext : public ExecContext {
public:
  /** Reads an integer register. */
  RegVal readIntRegOperand(const StaticInst *si, int idx) override {
    return this->readIntRegOperand(si->srcRegIdx(idx));
  }

  /** Sets an integer register to a value. */
  void setIntRegOperand(const StaticInst *si, int idx, RegVal val) override {
    this->setIntRegOperand(si->destRegIdx(idx), val);
  }

  /** Directly read/set the integer register, used to pass in arguments. **/
  RegVal readIntRegOperand(const RegId &reg) {
    assert(reg.isIntReg());
    // For RISCV, this is directly flattened.
    return this->intRegs[reg.index()];
  }
  void setIntRegOperand(const RegId &reg, RegVal val) {
    assert(reg.isIntReg());
    this->intRegs[reg.index()] = val;
  }

  /** @} */

  /**
   * @{
   * @name Floating Point Register Interfaces
   */

  /** Reads a floating point register in its binary format, instead
   * of by value. */
  RegVal readFloatRegOperandBits(const StaticInst *si, int idx) override {
    const RegId &reg = si->srcRegIdx(idx);
    assert(reg.isFloatReg());
    assert(reg.index() < TheISA::NumFloatRegs);
    return this->floatRegs[reg.index()];
  }

  /** Sets the bits of a floating point register of single width
   * to a binary value. */
  void setFloatRegOperandBits(const StaticInst *si, int idx,
                              RegVal val) override {
    const RegId &reg = si->destRegIdx(idx);
    assert(reg.isFloatReg());
    assert(reg.index() < TheISA::NumFloatRegs);
    this->floatRegs[reg.index()] = val;
  }

  /** Directly read/set the integer register, used to pass in arguments. **/
  RegVal readFloatRegOperand(const RegId &reg) {
    assert(reg.isFloatReg());
    assert(reg.index() < TheISA::NumFloatRegs);
    return this->floatRegs[reg.index()];
  }
  void setFloatRegOperand(const RegId &reg, RegVal val) {
    assert(reg.isFloatReg());
    assert(reg.index() < TheISA::NumFloatRegs);
    this->floatRegs[reg.index()] = val;
  }

  /** @} */

  /** Vector Register Interfaces. */
  /** @{ */
  /** Reads source vector register operand. */
  const VecRegContainer &readVecRegOperand(const StaticInst *si,
                                           int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Gets destination vector register operand for modification. */
  VecRegContainer &getWritableVecRegOperand(const StaticInst *si,
                                            int idx) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Sets a destination vector register operand to a value. */
  void setVecRegOperand(const StaticInst *si, int idx,
                        const VecRegContainer &val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  /** @} */

  /** Vector Register Lane Interfaces. */
  /** @{ */
  /** Reads source vector 8bit operand. */
  ConstVecLane8 readVec8BitLaneOperand(const StaticInst *si,
                                       int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Reads source vector 16bit operand. */
  ConstVecLane16 readVec16BitLaneOperand(const StaticInst *si,
                                         int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Reads source vector 32bit operand. */
  ConstVecLane32 readVec32BitLaneOperand(const StaticInst *si,
                                         int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Reads source vector 64bit operand. */
  ConstVecLane64 readVec64BitLaneOperand(const StaticInst *si,
                                         int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Write a lane of the destination vector operand. */
  /** @{ */
  void setVecLaneOperand(const StaticInst *si, int idx,
                         const LaneData<LaneSize::Byte> &val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void setVecLaneOperand(const StaticInst *si, int idx,
                         const LaneData<LaneSize::TwoByte> &val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void setVecLaneOperand(const StaticInst *si, int idx,
                         const LaneData<LaneSize::FourByte> &val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void setVecLaneOperand(const StaticInst *si, int idx,
                         const LaneData<LaneSize::EightByte> &val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  /** @} */

  /** Vector Elem Interfaces. */
  /** @{ */
  /** Reads an element of a vector register. */
  VecElem readVecElemOperand(const StaticInst *si, int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Sets a vector register to a value. */
  void setVecElemOperand(const StaticInst *si, int idx,
                         const VecElem val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  /** @} */

  /** Predicate registers interface. */
  /** @{ */
  /** Reads source predicate register operand. */
  const VecPredRegContainer &readVecPredRegOperand(const StaticInst *si,
                                                   int idx) const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Gets destination predicate register operand for modification. */
  VecPredRegContainer &getWritableVecPredRegOperand(const StaticInst *si,
                                                    int idx) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** Sets a destination predicate register operand to a value. */
  void setVecPredRegOperand(const StaticInst *si, int idx,
                            const VecPredRegContainer &val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  /** @} */

  /**
   * Condition Code Registers
   */
  RegVal readCCRegOperand(const StaticInst *si, int idx) override {
#ifdef ISA_HAS_CC_REGS
    const RegId &reg = si->srcRegIdx(idx);
    assert(reg.isCCReg());
    return this->ccRegs[this->flattenCCRegIdx(reg.index())];
#else
    panic("Tried to read a CC register.");
#endif
  }

  void setCCRegOperand(const StaticInst *si, int idx, RegVal val) override {
#ifdef ISA_HAS_CC_REGS
    const RegId &reg = si->destRegIdx(idx);
    assert(reg.isCCReg());
    this->ccRegs[this->flattenCCRegIdx(reg.index())] = val;
#else
    panic("Tried to set a CC register.");
#endif
  }

  /**
   * @{
   * @name Misc Register Interfaces
   */
  RegVal readMiscRegOperand(const StaticInst *si, int idx) override {
/**
 * Only support CSBase for Rdip.
 */
#if THE_ISA == X86_ISA
    const auto &reg = si->srcRegIdx(idx);
    assert(reg.isMiscReg());
    switch (reg.index()) {
    case X86ISA::MiscRegIndex::MISCREG_DS_EFF_BASE:
    case X86ISA::MiscRegIndex::MISCREG_CS_EFF_BASE: {
      return 0;
    }
    default: { panic("Unsupported MiscReg %d.\n", reg.index()); }
    }
#endif
    panic("FuncAddrExecContext does not implement this.");
  }
  void setMiscRegOperand(const StaticInst *si, int idx, RegVal val) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /**
   * Reads a miscellaneous register, handling any architectural
   * side effects due to reading that register.
   */
  RegVal readMiscReg(int misc_reg) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /**
   * Sets a miscellaneous register, handling any architectural
   * side effects due to writing that register.
   */
  void setMiscReg(int misc_reg, RegVal val) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** @} */

  /**
   * @{
   * @name PC Control
   */
  PCState pcState() const override { return this->pc; }
  void pcState(const PCState &val) override { this->pc = val; }
  /** @} */

  /**
   * @{
   * @name Memory Interface
   */
  /**
   * Perform an atomic memory read operation.  Must be overridden
   * for exec contexts that support atomic memory mode.  Not pure
   * since exec contexts that only support timing memory
   * mode need not override (though in that case this function
   * should never be called).
   */
  Fault readMem(Addr addr, uint8_t *data, unsigned int size,
                Request::Flags flags,
                const std::vector<bool> &byteEnable = std::vector<bool>()) {
    assert(this->virtProxy && "No virt port proxy.");
    if (!this->virtProxy->tryReadBlob(addr, data, size)) {
      panic("ExecContext::readMem() failed.\n");
    }
    return NoFault;
  }

  /**
   * Initiate a timing memory read operation.  Must be overridden
   * for exec contexts that support timing memory mode.  Not pure
   * since exec contexts that only support atomic memory
   * mode need not override (though in that case this function
   * should never be called).
   */
  Fault
  initiateMemRead(Addr addr, unsigned int size, Request::Flags flags,
                  const std::vector<bool> &byteEnable = std::vector<bool>()) {
    panic("ExecContext::initiateMemRead() should be overridden\n");
  }

  /**
   * For atomic-mode contexts, perform an atomic memory write operation.
   * For timing-mode contexts, initiate a timing memory write operation.
   */
  Fault
  writeMem(uint8_t *data, unsigned int size, Addr addr, Request::Flags flags,
           uint64_t *res,
           const std::vector<bool> &byteEnable = std::vector<bool>()) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /**
   * For atomic-mode contexts, perform an atomic AMO (a.k.a., Atomic
   * Read-Modify-Write Memory Operation)
   */
  Fault amoMem(Addr addr, uint8_t *data, unsigned int size,
               Request::Flags flags, AtomicOpFunctor *amo_op) {
    panic("ExecContext::amoMem() should be overridden\n");
  }

  /**
   * For timing-mode contexts, initiate an atomic AMO (atomic
   * read-modify-write memory operation)
   */
  Fault initiateMemAMO(Addr addr, unsigned int size, Request::Flags flags,
                       AtomicOpFunctor *amo_op) {
    panic("ExecContext::initiateMemAMO() should be overridden\n");
  }

  /**
   * Sets the number of consecutive store conditional failures.
   */
  void setStCondFailures(unsigned int sc_failures) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /**
   * Returns the number of consecutive store conditional failures.
   */
  unsigned int readStCondFailures() const override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** @} */

  /**
   * @{
   * @name SysCall Emulation Interfaces
   */

  /**
   * Executes a syscall specified by the callnum.
   */
  void syscall(int64_t callnum, Fault *fault) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** @} */

  /** Returns a pointer to the ThreadContext. */
  ThreadContext *tcBase() override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /**
   * @{
   * @name ARM-Specific Interfaces
   */

  bool readPredicate() const override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void setPredicate(bool val) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  bool readMemAccPredicate() const override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void setMemAccPredicate(bool val) override {
    panic("FuncAddrExecContext does not implement this.");
  }

  /** @} */

  /**
   * @{
   * @name X86-Specific Interfaces
   */

  /**
   * Invalidate a page in the DTLB <i>and</i> ITLB.
   */
  void demapPage(Addr vaddr, uint64_t asn) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void armMonitor(Addr address) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  bool mwait(PacketPtr pkt) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  void mwaitAtomic(ThreadContext *tc) override {
    panic("FuncAddrExecContext does not implement this.");
  }
  AddressMonitor *getAddrMonitor() override {
    panic("FuncAddrExecContext does not implement this.");
  }

  void clear() {
    for (auto &reg : this->intRegs) {
      reg = 0;
    }
    for (auto &reg : this->floatRegs) {
      reg = 0;
    }
    for (auto &reg : this->ccRegs) {
      reg = 0;
    }
    this->virtProxy = nullptr;
  }

  void setVirtProxy(PortProxy *virtProxy) {
    assert(!this->virtProxy && "VirtProxy already set.");
    this->virtProxy = virtProxy;
  }

protected:
  RegVal intRegs[TheISA::NumIntRegs];
  RegVal floatRegs[TheISA::NumFloatRegs];
#ifdef ISA_HAS_CC_REGS
  RegVal ccRegs[TheISA::NumCCRegs];
  int flattenCCRegIdx(int regIdx) const {
#if THE_ISA == X86_ISA
    int flatIndex = regIdx;
#elif THE_ISA == RISCV_ISA
    int flatIndex = regIdx;
#else
    panic("ExecFuncContext does not support this ISA.");
#endif
    assert(flatIndex < TheISA::NumCCRegs);
    return flatIndex;
  }
#endif
  TheISA::PCState pc;
  PortProxy *virtProxy = nullptr;
};

#endif