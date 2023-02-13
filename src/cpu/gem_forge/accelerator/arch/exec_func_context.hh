#ifndef __GEM_FORGE_EXEC_FUNC_CONTEXT_HH__
#define __GEM_FORGE_EXEC_FUNC_CONTEXT_HH__

#include "cpu/exec_context.hh"
#include "cpu/regfile.hh"

#if THE_ISA == X86_ISA
// ! Jesus I break the isolation.
#include "arch/x86/regs/misc.hh"
#endif

namespace gem5 {

/**
 * A taylored ExecContext that only provides integer register file,
 * for the address computation.
 */
class ExecFuncContext : public ExecContext {
public:
  RegVal getRegOperand(const StaticInst *si, int idx) {
    RegVal val = 0;
    this->getRegOperand(si, idx, &val);
    return val;
  }

  void getRegOperand(const StaticInst *si, int idx, void *val) {
    const auto &regId = si->srcRegIdx(idx);
    if (regId.is(InvalidRegClass)) {
      return;
    }
    const RegId reg = regId.flatten(*isa);

    const auto &regFile = regFiles[reg.classValue()];

    regFile->get(reg.index(), val);
  }

  void *getWritableRegOperand(const StaticInst *si, int idx) {

    const RegId &regId = si->destRegIdx(idx);
    const RegId reg = regId.flatten(*isa);
    auto &reg_file = regFiles[reg.classValue()];

    return reg_file->ptr(reg.index());
  }

  void setRegOperand(const StaticInst *si, int idx, RegVal val) {
    this->setRegOperand(si, idx, &val);
  }

  void setRegOperand(const StaticInst *si, int idx, const void *val);

  /** Directly read/set the integer register, used to pass in arguments. **/
  RegVal getRegOperand(const RegId &regId) {
    assert(!regId.is(InvalidRegClass));
    const RegId reg = regId.flatten(*isa);
    const RegIndex idx = reg.index();
    const auto &regFile = regFiles[reg.classValue()];
    return regFile->reg(idx);
  }

  void setRegOperand(const RegId &regId, RegVal val) {
    const RegId reg = regId.flatten(*isa);
    const RegIndex idx = reg.index();
    const auto &regFile = regFiles[reg.classValue()];
    regFile->set(idx, &val);
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
    assert(reg.classValue() == RegClassType::MiscRegClass);
    switch (reg.index()) {
    case X86ISA::misc_reg::SsEffBase:
    case X86ISA::misc_reg::DsEffBase:
    case X86ISA::misc_reg::CsEffBase: {
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
  const PCStateBase &pcState() const override { return *this->_pcState; }
  void pcState(const PCStateBase &val) override { set(this->_pcState, val); }
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
  Fault
  readMem(Addr addr, uint8_t *data, unsigned int size, Request::Flags flags,
          const std::vector<bool> &byteEnable = std::vector<bool>()) override {
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
  Fault initiateMemRead(Addr addr, unsigned int size, Request::Flags flags,
                        const std::vector<bool> &byteEnable) override {
    panic("ExecContext::initiateMemRead() should be overridden\n");
  }

  /**
   * Initiate a memory management command with no valid address.
   * Currently, these instructions need to bypass squashing in the O3 model
   * Examples include HTM commands and TLBI commands.
   * e.g. tell Ruby we're starting/stopping a HTM transaction,
   *      or tell Ruby to issue a TLBI operation
   */
  Fault initiateMemMgmtCmd(Request::Flags flags) override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
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

  // hardware transactional memory
  uint64_t newHtmTransactionUid() const override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  uint64_t getHtmTransactionUid() const override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  bool inHtmTransactionalState() const override {
    panic("FuncAddrExecContext does not implement this %s.", __func__);
  }
  uint64_t getHtmTransactionalDepth() const override {
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
    for (auto &regFile : this->regFiles) {
      regFile->clear();
    }
    this->virtProxy = nullptr;
  }

  void init(BaseISA *const _isa) {
    if (this->isa) {
      // Already initialized.
      return;
    }
    this->isa = _isa;
    for (const auto &regClass : this->isa->regClasses()) {
      this->regFiles.push_back(std::make_unique<RegFile>(*regClass));
    }
  }

  void setVirtProxy(PortProxy *virtProxy) {
    assert(!this->virtProxy && "VirtProxy already set.");
    this->virtProxy = virtProxy;
  }

protected:
  BaseISA *isa = nullptr;
  std::vector<std::unique_ptr<RegFile>> regFiles;
  std::unique_ptr<PCStateBase> _pcState;
  PortProxy *virtProxy = nullptr;
};

} // namespace gem5

#endif