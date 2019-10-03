#include "timing_simple_cpu_delegator.hh"

#if THE_ISA == RISCV_ISA
#include "cpu/gem_forge/accelerator/arch/riscv/gem_forge_isa_handler.hh"
#else
#error "Unsupported ISA."
#endif

class TimingSimpleCPUDelegator::Impl {
public:
  Impl(TimingSimpleCPU *_cpu, TimingSimpleCPUDelegator *_cpuDelegator)
      : cpu(_cpu), state(StateE::BEFORE_DISPATCH), curSeqNum(1),
        isaHandler(_cpuDelegator) {}

  TimingSimpleCPU *cpu;
  /**
   * Sanity check state.
   */
  enum StateE {
    BEFORE_DISPATCH,
    BEFORE_EXECUTE,
    BEFORE_COMMIT,
  };
  StateE state;
  /**
   * For TimingSimpleCPU, it is trivial to maintain a sequence number.
   * This starts from 1, as 0 is reserved for invalid.
   */
  uint64_t curSeqNum;

  TheISA::GemForgeISAHandler isaHandler;

  std::string traceExtraFolder;

  Process *getProcess() {
    assert(this->cpu->activeThreads.size() == 1 &&
           "SMT not supported in GemForge yet.");
    // Crazy oracle access chain.
    auto threadInfo =
        this->cpu->threadInfo.at(this->cpu->activeThreads.front());
    auto thread = threadInfo->thread;
    auto process = thread->getProcessPtr();
    return process;
  }
};

/**************************************************************************
 * TimingSimpleCPUDelegator.
 *************************************************************************/

TimingSimpleCPUDelegator::TimingSimpleCPUDelegator(TimingSimpleCPU *_cpu)
    : GemForgeCPUDelegator(CPUTypeE::TIMING_SIMPLE, _cpu),
      pimpl(new Impl(_cpu, this)) {}
TimingSimpleCPUDelegator::~TimingSimpleCPUDelegator() = default;

bool TimingSimpleCPUDelegator::canDispatch(StaticInstPtr staticInst,
                                           ExecContext &xc) {
  assert(pimpl->state == Impl::StateE::BEFORE_DISPATCH);
  TheISA::GemForgeDynInstInfo dynInfo(pimpl->curSeqNum, xc.pcState(),
                                      staticInst.get(), xc.tcBase());
  return pimpl->isaHandler.canDispatch(dynInfo);
}

void TimingSimpleCPUDelegator::dispatch(StaticInstPtr staticInst,
                                        ExecContext &xc) {
  assert(pimpl->state == Impl::StateE::BEFORE_DISPATCH);
  TheISA::GemForgeDynInstInfo dynInfo(pimpl->curSeqNum, xc.pcState(),
                                      staticInst.get(), xc.tcBase());
  /**
   * SimpleTimingCPU never really all cause a RAW misspeculation in LSQ,
   * so we ignore any extra LQCallbacks.
   *
   * TODO: Correctly handle the StreamLoad, which is now a MemRef inst.
   */
  GemForgeLQCallbackList extraLQCallbacks;
  bool isGemForgeLoad;
  pimpl->isaHandler.dispatch(dynInfo, extraLQCallbacks, isGemForgeLoad);
  pimpl->state = Impl::StateE::BEFORE_EXECUTE;
}

bool TimingSimpleCPUDelegator::canExecute(StaticInstPtr staticInst,
                                          ExecContext &xc) {
  assert(pimpl->state == Impl::StateE::BEFORE_EXECUTE);
  TheISA::GemForgeDynInstInfo dynInfo(pimpl->curSeqNum, xc.pcState(),
                                      staticInst.get(), xc.tcBase());
  return pimpl->isaHandler.canExecute(dynInfo);
}

void TimingSimpleCPUDelegator::execute(StaticInstPtr staticInst,
                                       ExecContext &xc) {
  assert(pimpl->state == Impl::StateE::BEFORE_EXECUTE);
  TheISA::GemForgeDynInstInfo dynInfo(pimpl->curSeqNum, xc.pcState(),
                                      staticInst.get(), xc.tcBase());
  pimpl->isaHandler.execute(dynInfo, xc);
  pimpl->state = Impl::StateE::BEFORE_COMMIT;
}

void TimingSimpleCPUDelegator::commit(StaticInstPtr staticInst,
                                      ExecContext &xc) {
  assert(pimpl->state == Impl::StateE::BEFORE_COMMIT);
  TheISA::GemForgeDynInstInfo dynInfo(pimpl->curSeqNum, xc.pcState(),
                                      staticInst.get(), xc.tcBase());
  pimpl->isaHandler.commit(dynInfo);
  pimpl->state = Impl::StateE::BEFORE_DISPATCH;
  pimpl->curSeqNum++;
}

void TimingSimpleCPUDelegator::storeTo(Addr vaddr, int size) {
  pimpl->isaHandler.storeTo(vaddr, size);
}

const std::string &TimingSimpleCPUDelegator::getTraceExtraFolder() const {
  // Always assume that the binary is in the TraceExtraFolder.
  if (pimpl->traceExtraFolder.empty()) {
    auto process = pimpl->getProcess();
    const auto &executable = process->executable;
    auto sepPos = executable.rfind('/');
    if (sepPos == std::string::npos) {
      // Not found.
      pimpl->traceExtraFolder = ".";
    } else {
      pimpl->traceExtraFolder = executable.substr(0, sepPos);
    }
  }
  return pimpl->traceExtraFolder;
}

Addr TimingSimpleCPUDelegator::translateVAddrOracle(Addr vaddr) {
  auto process = pimpl->getProcess();
  auto pTable = process->pTable;
  Addr paddr;
  if (pTable->translate(vaddr, paddr)) {
    return paddr;
  }
  // TODO: Let the caller handle this.
  panic("Translate vaddr failed %#x.", vaddr);
  return paddr;
}

void TimingSimpleCPUDelegator::sendRequest(PacketPtr pkt) {
  // The CPU's port should already be a GemForgeDcachePort.
  assert(dynamic_cast<TimingSimpleCPU::GemForgeDcachePort *>(
             pimpl->cpu->dcachePort.get()) &&
         "GemForgeCPUDelegator::sendRequest called when the DcachePort is not "
         "a GemForge port.");
  auto succeed = pimpl->cpu->dcachePort->sendTimingReqVirtual(pkt);
  assert(succeed && "GemForgePort should always succeed on sending TimingReq.");
}
