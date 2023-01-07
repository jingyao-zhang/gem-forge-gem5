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
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /** Gets destination vector register operand for modification. */
  VecRegContainer &getWritableVecRegOperand(const StaticInst *si,
                                            int idx) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /** Sets a destination vector register operand to a value. */
  void setVecRegOperand(const StaticInst *si, int idx,
                        const VecRegContainer &val) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  /** @} */

  /** Vector Register Lane Interfaces. */
  /** @{ */
  /** Reads source vector 8bit operand. */
  ConstVecLane8 readVec8BitLaneOperand(const StaticInst *si,
                                       int idx) const override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /** Reads source vector 16bit operand. */
  ConstVecLane16 readVec16BitLaneOperand(const StaticInst *si,
                                         int idx) const override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /** Reads source vector 32bit operand. */
  ConstVecLane32 readVec32BitLaneOperand(const StaticInst *si,
                                         int idx) const override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /** Reads source vector 64bit operand. */
  ConstVecLane64 readVec64BitLaneOperand(const StaticInst *si,
                                         int idx) const override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /** Write a lane of the destination vector operand. */
  /** @{ */
  void setVecLaneOperand(const StaticInst *si, int idx,
                         const LaneData<LaneSize::Byte> &val) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  void setVecLaneOperand(const StaticInst *si, int idx,
                         const LaneData<LaneSize::TwoByte> &val) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  void setVecLaneOperand(const StaticInst *si, int idx,
                         const LaneData<LaneSize::FourByte> &val) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  void setVecLaneOperand(const StaticInst *si, int idx,
                         const LaneData<LaneSize::EightByte> &val) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  /** @} */

  /** Vector Elem Interfaces. */
  /** @{ */
  /** Reads an element of a vector register. */
  VecElem readVecElemOperand(const StaticInst *si, int idx) const override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /** Sets a vector register to a value. */
  void setVecElemOperand(const StaticInst *si, int idx,
                         const VecElem val) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  /** @} */

  /** Predicate registers interface. */
  /** @{ */
  /** Reads source predicate register operand. */
  const VecPredRegContainer &readVecPredRegOperand(const StaticInst *si,
                                                   int idx) const override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /** Gets destination predicate register operand for modification. */
  VecPredRegContainer &getWritableVecPredRegOperand(const StaticInst *si,
                                                    int idx) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /** Sets a destination predicate register operand to a value. */
  void setVecPredRegOperand(const StaticInst *si, int idx,
                            const VecPredRegContainer &val) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  /** @} */

  /**
   * Condition Code Registers
   */
  RegVal readCCRegOperand(const StaticInst *si, int idx) override {
    const RegId &reg = si->srcRegIdx(idx);
    assert(reg.isCCReg());
    return this->ccRegs[this->flattenCCRegIdx(reg.index())];
  }

  RegVal readCCRegOperand(const RegId &reg) {
    assert(reg.isCCReg());
    return this->ccRegs[this->flattenCCRegIdx(reg.index())];
  }

  void setCCRegOperand(const StaticInst *si, int idx, RegVal val) override {
    const RegId &reg = si->destRegIdx(idx);
    assert(reg.isCCReg());
    this->ccRegs[this->flattenCCRegIdx(reg.index())] = val;
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
    case X86ISA::MiscRegIndex::MISCREG_SS_EFF_BASE:
    case X86ISA::MiscRegIndex::MISCREG_DS_EFF_BASE:
    case X86ISA::MiscRegIndex::MISCREG_CS_EFF_BASE: {
      return 0;
    }
    default: {
      panic("Unsupported MiscReg %d.\n", reg.index());
    }
    }
#endif
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  void setMiscRegOperand(const StaticInst *si, int idx, RegVal val) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /**
   * Reads a miscellaneous register, handling any architectural
   * side effects due to reading that register.
   */
  RegVal readMiscReg(int misc_reg) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /**
   * Sets a miscellaneous register, handling any architectural
   * side effects due to writing that register.
   */
  void setMiscReg(int misc_reg, RegVal val) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
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
   * Special case for the stack. We maintain a fake stack and
   * intercept all accesses to the stack addresses here.
   * For now just keep a extremly small stack.
   */
  static constexpr int FAKE_STACK_SIZE = 1024;
  static constexpr int FAKE_STACK_RED_ZONE_SIZE = 1024;
  static constexpr Addr FAKE_STACK_TOP_VADDR = 0x7fffffffff00;
  static constexpr Addr FAKE_STACK_BASE_VADDR =
      FAKE_STACK_TOP_VADDR - FAKE_STACK_SIZE;
  static constexpr Addr FAKE_STACK_BOTTOM_VADDR =
      FAKE_STACK_BASE_VADDR - FAKE_STACK_RED_ZONE_SIZE;
  char fakeStack[FAKE_STACK_SIZE];
  bool isFakeStackVAddr(Addr vaddr) {
    return vaddr < FAKE_STACK_TOP_VADDR && vaddr >= FAKE_STACK_BOTTOM_VADDR;
  }
  bool isValidFakeStackVAddr(Addr vaddr) {
    return vaddr < FAKE_STACK_TOP_VADDR && vaddr >= FAKE_STACK_BASE_VADDR;
  }
  template <typename T> T *getFakeStackPtr(Addr vaddr) {
    assert(isValidFakeStackVAddr(vaddr) && "Invalid FakeStackVAddr.");
    char *ret = &fakeStack[vaddr - FAKE_STACK_BASE_VADDR];
    return reinterpret_cast<T *>(ret);
  }
  template <typename T> void storeFakeStack(Addr vaddr, T value) {
    auto ptr = getFakeStackPtr<T>(vaddr);
    *ptr = value;
  }
  template <typename T> T readFakeStack(Addr vaddr) {
    return *getFakeStackPtr<T>(vaddr);
  }
  void readFakeStack(Addr vaddr, uint8_t *data, unsigned int size) {
    auto ptr = getFakeStackPtr<uint8_t>(vaddr);
    for (int i = 0; i < size; ++i) {
      data[i] = ptr[i];
    }
  }

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
    if (this->isFakeStackVAddr(addr)) {
      this->readFakeStack(addr, data, size);
      return NoFault;
    }
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
    assert(this->virtProxy && "No virt port proxy.");
    if (this->isFakeStackVAddr(addr)) {
      this->readFakeStack(addr, data, size);
      assert(size == 8);
      uint64_t value = *reinterpret_cast<uint64_t *>(data);
      this->storeFakeStack(addr, value);
      return NoFault;
    }
    panic("FuncAddrExecContext does not implement this %s.", __func__);
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
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /**
   * Returns the number of consecutive store conditional failures.
   */
  unsigned int readStCondFailures() const override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /** @} */

  /**
   * @{
   * @name SysCall Emulation Interfaces
   */

  /**
   * Executes a syscall specified by the callnum.
   */
  void syscall(Fault *fault) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /** @} */

  /** Returns a pointer to the ThreadContext. */
  ThreadContext *tcBase() const override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }

  /**
   * @{
   * @name ARM-Specific Interfaces
   */

  bool readPredicate() const override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  void setPredicate(bool val) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  bool readMemAccPredicate() const override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  void setMemAccPredicate(bool val) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
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
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  void armMonitor(Addr address) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  bool mwait(PacketPtr pkt) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  void mwaitAtomic(ThreadContext *tc) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  AddressMonitor *getAddrMonitor() override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
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
  TheISA::PCState pc;
  PortProxy *virtProxy = nullptr;
};

#endif