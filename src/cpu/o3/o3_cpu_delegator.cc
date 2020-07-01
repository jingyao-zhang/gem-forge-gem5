#include "o3_cpu_delegator.hh"

#include "cpu/gem_forge/accelerator/arch/gem_forge_isa_handler.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/impl.hh"
#include "debug/O3CPUDelegator.hh"
#include "debug/O3CPUDelegatorDump.hh"

#define INST_DPRINTF(inst, format, args...)                                    \
  DPRINTF(O3CPUDelegator, "[%s]: " format, *(inst), ##args)
#define INST_PANIC(inst, format, args...)                                      \
  panic("[%s]: " format, *(inst), ##args)

template <class CPUImpl> class DefaultO3CPUDelegator<CPUImpl>::Impl {
public:
  Impl(O3CPU *_cpu, DefaultO3CPUDelegator *_cpuDelegator)
      : cpu(_cpu), cpuDelegator(_cpuDelegator), isaHandler(_cpuDelegator) {}

  O3CPU *cpu;
  DefaultO3CPUDelegator<CPUImpl> *cpuDelegator;

  GemForgeISAHandler isaHandler;

  // Cache of the traceExtraFolder.
  std::string traceExtraFolder;

  /**
   * For simplicity, we maintain our own queue of infly instruction.
   */
  std::deque<DynInstPtr> inflyInstQueue;

  Process *getProcess() {
    assert(this->cpu->thread.size() == 1 &&
           "SMT not supported in O3CPUDelegator yet.");
    // Crazy oracle access chain.
    auto thread = this->cpu->thread.front();
    auto process = thread->getProcessPtr();
    return process;
  }

  uint64_t getInstSeqNum(const DynInstPtr &dynInstPtr) const {
    auto seqNum = dynInstPtr->seqNum;
    assert(seqNum != 0 && "GemForge assumes SeqNum 0 is reserved as invalid.");
    return seqNum;
  }

  ThreadContext *getThreadContext(const DynInstPtr &dynInstPtr) const {
    ThreadID threadId = dynInstPtr->threadNumber;
    ThreadContext *thread = cpu->getContext(threadId);
    return thread;
  }

  GemForgeDynInstInfo createDynInfo(const DynInstPtr &dynInstPtr) const {
    if (dynInstPtr->isSquashed()) {
      panic("Should not be a squashed inst.");
    }
    GemForgeDynInstInfo dynInfo(
        this->getInstSeqNum(dynInstPtr), dynInstPtr->pcState(),
        dynInstPtr->staticInst.get(), this->getThreadContext(dynInstPtr));
    return dynInfo;
  }
};

/**********************************************************************
 * O3CPUDelegator.
 *********************************************************************/

template <class CPUImpl>
DefaultO3CPUDelegator<CPUImpl>::DefaultO3CPUDelegator(O3CPU *_cpu)
    : GemForgeCPUDelegator(CPUTypeE::O3, _cpu), pimpl(new Impl(_cpu, this)) {}

template <class CPUImpl>
DefaultO3CPUDelegator<CPUImpl>::~DefaultO3CPUDelegator() = default;

/*********************************************************************
 * Interface to GemForge.
 *********************************************************************/
template <class CPUImpl>
const std::string &DefaultO3CPUDelegator<CPUImpl>::getTraceExtraFolder() const {
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

template <class CPUImpl>
bool DefaultO3CPUDelegator<CPUImpl>::translateVAddrOracle(Addr vaddr,
                                                          Addr &paddr) {
  auto process = pimpl->getProcess();
  auto pTable = process->pTable;
  if (!pTable->translate(vaddr, paddr)) {
    // Due to the new MemState class and lazy allocation, it's possible
    // that this page has not allocated. However, we want to simplify
    // our life as before, so try to fix it?
    if (process->fixupFault(vaddr)) {
      // Try again.
      return pTable->translate(vaddr, vaddr);
    }
    return false;
  }
  return true;
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::sendRequest(PacketPtr pkt) {
  // So far we don't handle aliasing, so just send.
  auto &lsq = pimpl->cpu->getIEW()->ldstQueue;
  assert(lsq.getDataPortPtr()->sendTimingReqVirtual(pkt, false /* isCore */));

  //   // If this is not a load request, we should send immediately.
  //   // e.g. StreamConfig/End packet.
  //   if (pkt->cmd != MemCmd::ReadReq) {
  //     auto &lsq = pimpl->cpu->pipeline->execute.getLSQ();
  //     assert(lsq.dcachePort->sendTimingReqVirtual(pkt, false /* isCore */));
  //     return;
  //   }
  //   auto lineBytes = this->cacheLineSize();
  //   if ((pkt->getAddr() % lineBytes) + pkt->getSize() > lineBytes) {
  //     panic("Multi-line packet paddr %#x size %d.", pkt->getAddr(),
  //           pkt->getSize());
  //   }

  //   pimpl->pendingPkts.push_back(pkt);
  //   if (!pimpl->drainPendingPacketsEvent.scheduled()) {
  //     this->drainPendingPackets();
  //   }
}

template <class CPUImpl>
bool DefaultO3CPUDelegator<CPUImpl>::canDispatch(DynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  auto ret = pimpl->isaHandler.canDispatch(dynInfo);
  if (!ret) {
    INST_DPRINTF(dynInstPtr, "Cannot dispatch.\n");
  }
  return ret;
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::dispatch(DynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  INST_DPRINTF(dynInstPtr, "Dispatch.\n");
  GemForgeLQCallbackList extraLQCallbacks;
  bool isGemForgeLoad = false;
  pimpl->isaHandler.dispatch(dynInfo, extraLQCallbacks, isGemForgeLoad);
  pimpl->inflyInstQueue.push_back(dynInstPtr);
  /**
   * GemForge interation with LSQ is not supported yet.
   * So far we always turn them into NonMemRef instructions.
   */
  if (isGemForgeLoad) {
    assert(dynInstPtr->isMemRef() && "Should be MemRef for GemForgeLoad.");
    assert(dynInstPtr->isLoad() && "Should be Load for GemForgeLoad.");
    dynInstPtr->forceNotMemRef = true;
  }

  if (extraLQCallbacks.front()) {
  } else if (isGemForgeLoad) {
  }
}

template <class CPUImpl>
bool DefaultO3CPUDelegator<CPUImpl>::canExecute(DynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  auto ret = pimpl->isaHandler.canExecute(dynInfo);
  if (!ret) {
    INST_DPRINTF(dynInstPtr, "Cannot execute.\n");
  }
  return ret;
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::execute(DynInstPtr &dynInstPtr) {
  INST_DPRINTF(dynInstPtr, "Execute.\n");
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  // DynInst is also ExecContext.
  pimpl->isaHandler.execute(dynInfo, *dynInstPtr);
}

template <class CPUImpl>
bool DefaultO3CPUDelegator<CPUImpl>::canCommit(const DynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  auto ret = pimpl->isaHandler.canCommit(dynInfo);
  if (!ret) {
    INST_DPRINTF(dynInstPtr, "Cannot commit.\n");
  }
  return ret;
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::commit(const DynInstPtr &dynInstPtr) {
  INST_DPRINTF(dynInstPtr, "Commit.\n");
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  assert(!pimpl->inflyInstQueue.empty() &&
         "Empty InflyInstQueue to commit from.");

  auto &frontInst = pimpl->inflyInstQueue.front();
  if (frontInst != dynInstPtr) {
    INST_PANIC(dynInstPtr, "Commit mismatch with InflyInstQueue front %s.",
               *frontInst);
  }
  pimpl->inflyInstQueue.pop_front();
  pimpl->isaHandler.commit(dynInfo);
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::squash(InstSeqNum squashSeqNum) {
  auto &inflyInstQueue = pimpl->inflyInstQueue;
  while (!inflyInstQueue.empty()) {
    auto &inst = inflyInstQueue.back();
    auto seqNum = pimpl->getInstSeqNum(inst);
    if (seqNum <= squashSeqNum) {
      // We are done.
      break;
    }
    INST_DPRINTF(inst, "Rewind.\n");
    auto dynInfo = pimpl->createDynInfo(inst);
    pimpl->isaHandler.rewind(dynInfo);
    inflyInstQueue.pop_back();
  }
}

template class DefaultO3CPUDelegator<O3CPUImpl>;