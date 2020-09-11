#include "atomic_simple_cpu_delegator.hh"

#include "cpu/gem_forge/gem_forge_packet_handler.hh"

class AtomicSimpleCPUDelegator::Impl {
public:
  Impl(AtomicSimpleCPU *_cpu, AtomicSimpleCPUDelegator *_cpuDelegator)
      : cpu(_cpu), state(StateE::BEFORE_DISPATCH), curSeqNum(1) {}

  AtomicSimpleCPU *cpu;
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
   * For AtomicSimpleCPU, it is trivial to maintain a sequence number.
   * This starts from 1, as 0 is reserved for invalid.
   */
  uint64_t curSeqNum;

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
 * AtomicSimpleCPUDelegator.
 *************************************************************************/

AtomicSimpleCPUDelegator::AtomicSimpleCPUDelegator(AtomicSimpleCPU *_cpu)
    : GemForgeCPUDelegator(CPUTypeE::ATOMIC_SIMPLE, _cpu),
      pimpl(new Impl(_cpu, this)) {}
AtomicSimpleCPUDelegator::~AtomicSimpleCPUDelegator() = default;

bool AtomicSimpleCPUDelegator::canDispatch(StaticInstPtr staticInst,
                                           ExecContext &xc) {
  assert(pimpl->state == Impl::StateE::BEFORE_DISPATCH);
  GemForgeDynInstInfo dynInfo(pimpl->curSeqNum, xc.pcState(), staticInst.get(),
                              xc.tcBase());
  return isaHandler->canDispatch(dynInfo);
}

void AtomicSimpleCPUDelegator::dispatch(StaticInstPtr staticInst,
                                        ExecContext &xc) {
  assert(pimpl->state == Impl::StateE::BEFORE_DISPATCH);
  GemForgeDynInstInfo dynInfo(pimpl->curSeqNum, xc.pcState(), staticInst.get(),
                              xc.tcBase());
  /**
   * SimpleAtomicCPU never really all cause a RAW misspeculation in LSQ,
   * so we ignore any extra LQCallbacks.
   *
   * TODO: Correctly handle the StreamLoad, which is now a MemRef inst.
   */
  GemForgeLQCallbackList extraLQCallbacks;
  bool isGemForgeLoad;
  isaHandler->dispatch(dynInfo, extraLQCallbacks, isGemForgeLoad);
  pimpl->state = Impl::StateE::BEFORE_EXECUTE;
}

bool AtomicSimpleCPUDelegator::canExecute(StaticInstPtr staticInst,
                                          ExecContext &xc) {
  assert(pimpl->state == Impl::StateE::BEFORE_EXECUTE);
  GemForgeDynInstInfo dynInfo(pimpl->curSeqNum, xc.pcState(), staticInst.get(),
                              xc.tcBase());
  return isaHandler->canExecute(dynInfo);
}

void AtomicSimpleCPUDelegator::execute(StaticInstPtr staticInst,
                                       ExecContext &xc) {
  assert(pimpl->state == Impl::StateE::BEFORE_EXECUTE);
  GemForgeDynInstInfo dynInfo(pimpl->curSeqNum, xc.pcState(), staticInst.get(),
                              xc.tcBase());
  isaHandler->execute(dynInfo, xc);
  pimpl->state = Impl::StateE::BEFORE_COMMIT;
}

bool AtomicSimpleCPUDelegator::canCommit(StaticInstPtr staticInst,
                                         ExecContext &xc) {
  assert(pimpl->state == Impl::StateE::BEFORE_COMMIT);
  GemForgeDynInstInfo dynInfo(pimpl->curSeqNum, xc.pcState(), staticInst.get(),
                              xc.tcBase());
  return isaHandler->canCommit(dynInfo);
}

void AtomicSimpleCPUDelegator::commit(StaticInstPtr staticInst,
                                      ExecContext &xc) {
  assert(pimpl->state == Impl::StateE::BEFORE_COMMIT);
  GemForgeDynInstInfo dynInfo(pimpl->curSeqNum, xc.pcState(), staticInst.get(),
                              xc.tcBase());
  isaHandler->commit(dynInfo);
  pimpl->state = Impl::StateE::BEFORE_DISPATCH;
  pimpl->curSeqNum++;
}

void AtomicSimpleCPUDelegator::storeTo(Addr vaddr, int size) {
  isaHandler->storeTo(vaddr, size);
}

const std::string &AtomicSimpleCPUDelegator::getTraceExtraFolder() const {
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

bool AtomicSimpleCPUDelegator::translateVAddrOracle(Addr vaddr, Addr &paddr) {
  auto process = pimpl->getProcess();
  auto pTable = process->pTable;
  if (!pTable->translate(vaddr, paddr)) {
    // Due to the new MemState class and lazy allocation, it's possible
    // that this page has not allocated. However, we want to simplify
    // our life as before, so try to fix it?
    if (process->fixupFault(vaddr)) {
      // Try again.
      return pTable->translate(vaddr, paddr);
    }
    return false;
  }
  return true;
}

void AtomicSimpleCPUDelegator::sendRequest(PacketPtr pkt) {
  // In atomic mode we can directly send the packet.
  assert(GemForgePacketHandler::isGemForgePacket(pkt));
  GemForgePacketHandler::issueToMemory(this, pkt);
  auto latency = pimpl->cpu->sendPacket(pimpl->cpu->dcachePort, pkt);
  // Just stop the compiler complaining about unused variable.
  (void)latency;
  // For simplicity now we immediately send back response.
  GemForgePacketHandler::handleGemForgePacketResponse(this, pkt);
}

InstSeqNum AtomicSimpleCPUDelegator::getInstSeqNum() const {
  return pimpl->curSeqNum;
}

void AtomicSimpleCPUDelegator::setInstSeqNum(InstSeqNum seqNum) {
  pimpl->curSeqNum = seqNum;
}