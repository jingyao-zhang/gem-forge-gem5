#include "minor_cpu_delegator.hh"

#include "debug/MinorCPUDelegator.hh"

#if THE_ISA == RISCV_ISA
#include "cpu/gem_forge/accelerator/arch/riscv/gem_forge_isa_handler.hh"
#else
#error "Unsupported ISA"
#endif

#define INST_DPRINTF(inst, format, args...)                                    \
  DPRINTF(MinorCPUDelegator, "[%s]: " format, *(inst), ##args)
#define INST_LOG(log, inst, format, args...)                                   \
  log("[%s]: " format, *(inst), ##args)

class MinorCPUDelegator::Impl {
public:
  Impl(MinorCPU *_cpu, MinorCPUDelegator *_cpuDelegator)
      : cpu(_cpu), cpuDelegator(_cpuDelegator), isaHandler(_cpuDelegator) {}

  MinorCPU *cpu;
  MinorCPUDelegator *cpuDelegator;

  TheISA::GemForgeISAHandler isaHandler;

  // Cache of the traceExtraFolder.
  std::string traceExtraFolder;

  /**
   * For simplicity, we maintain our own queue of infly instruction.
   */
  std::deque<Minor::MinorDynInstPtr> inflyInstQueue;

  /**
   * Current streamSeqNum.
   */
  InstSeqNum currentStreamSeqNum = Minor::InstId::firstExecSeqNum;

  Process *getProcess() {
    assert(this->cpu->threads.size() == 1 &&
           "SMT not supported in GemForge yet.");
    // Crazy oracle access chain.
    auto thread = this->cpu->threads.front();
    auto process = thread->getProcessPtr();
    return process;
  }

  uint64_t getInstSeqNum(Minor::MinorDynInstPtr &dynInstPtr) const {
    auto seqNum = dynInstPtr->id.execSeqNum;
    assert(seqNum != 0 && "GemForge assumes SeqNum 0 is reserved as invalid.");
    return seqNum;
  }

  ThreadContext *getThreadContext(Minor::MinorDynInstPtr &dynInstPtr) const {
    ThreadID thread_id = dynInstPtr->id.threadId;
    ThreadContext *thread = cpu->getContext(thread_id);
    return thread;
  }

  TheISA::GemForgeDynInstInfo
  createDynInfo(Minor::MinorDynInstPtr &dynInstPtr) const {
    assert(dynInstPtr->isInst() && "Should be a real inst.");
    assert(dynInstPtr->id.streamSeqNum == this->currentStreamSeqNum &&
           "Mismatched streamSeqNum.");
    TheISA::GemForgeDynInstInfo dynInfo(
        this->getInstSeqNum(dynInstPtr), dynInstPtr->pc,
        dynInstPtr->staticInst.get(), this->getThreadContext(dynInstPtr));
    return dynInfo;
  }
};

/**********************************************************************
 * MinorCPUDelegator.
 *********************************************************************/

MinorCPUDelegator::MinorCPUDelegator(MinorCPU *_cpu)
    : GemForgeCPUDelegator(CPUTypeE::MINOR, _cpu), pimpl(new Impl(_cpu, this)) {
}
MinorCPUDelegator::~MinorCPUDelegator() = default;

bool MinorCPUDelegator::canDispatch(Minor::MinorDynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  auto ret = pimpl->isaHandler.canDispatch(dynInfo);
  if (!ret) {
    INST_DPRINTF(dynInstPtr, "Cannot dispatch.\n");
  }
  return ret;
}

void MinorCPUDelegator::dispatch(Minor::MinorDynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  INST_DPRINTF(dynInstPtr, "Dispatch.\n");
  pimpl->isaHandler.dispatch(dynInfo);
  pimpl->inflyInstQueue.push_back(dynInstPtr);
}

bool MinorCPUDelegator::canExecute(Minor::MinorDynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  auto ret = pimpl->isaHandler.canExecute(dynInfo);
  if (!ret) {
    INST_DPRINTF(dynInstPtr, "Cannot execute.\n");
  }
  return ret;
}

void MinorCPUDelegator::execute(Minor::MinorDynInstPtr &dynInstPtr,
                                ExecContext &xc) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  INST_DPRINTF(dynInstPtr, "Execute.\n");
  pimpl->isaHandler.execute(dynInfo, xc);
}

void MinorCPUDelegator::commit(Minor::MinorDynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  INST_DPRINTF(dynInstPtr, "Commit.\n");
  assert(!pimpl->inflyInstQueue.empty() &&
         "Empty inflyInstQueue to commit from.");
  auto &frontInst = pimpl->inflyInstQueue.front();
  if (pimpl->inflyInstQueue.front() != dynInstPtr) {
    INST_LOG(panic, dynInstPtr, "Commit mismatch inflyInstQueue front %s.",
             *frontInst);
  }
  pimpl->inflyInstQueue.pop_front();
  pimpl->isaHandler.commit(dynInfo);
}

void MinorCPUDelegator::streamChange(InstSeqNum newStreamSeqNum) {
  // Rewind the inflyInstQueue.
  auto &inflyInstQueue = pimpl->inflyInstQueue;
  while (!inflyInstQueue.empty() &&
         inflyInstQueue.back()->id.streamSeqNum != newStreamSeqNum) {
    // This needs to be rewind.
    auto &misspeculatedInst = inflyInstQueue.back();
    auto dynInfo = pimpl->createDynInfo(misspeculatedInst);
    pimpl->isaHandler.rewind(dynInfo);
    inflyInstQueue.pop_back();
  }
  pimpl->currentStreamSeqNum = newStreamSeqNum;
}

const std::string &MinorCPUDelegator::getTraceExtraFolder() const {
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

Addr MinorCPUDelegator::translateVAddrOracle(Addr vaddr) {
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

void MinorCPUDelegator::sendRequest(PacketPtr pkt) {
  hack("MinorCPUDelegator::sendRequest not yet supported.");
  // So far ignore it for debug purpose.
}
