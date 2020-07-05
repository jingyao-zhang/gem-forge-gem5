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

  /**
   * Store the LQ callbacks before they are really marked vaddr ready
   * and tracked by the LSQ.
   */
  std::unordered_map<InstSeqNum, GemForgeLQCallbackList> preLSQ;

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

  bool isInPreLSQ(const DynInstPtr &dynInstPtr) {
    auto seqNum = this->getInstSeqNum(dynInstPtr);
    return this->preLSQ.count(seqNum);
  }

  /**
   * Check if an instruction is in PreLSQ and its addr/size is ready.
   */
  bool isAddrSizeReady(const DynInstPtr &dynInstPtr) {
    auto seqNum = this->getInstSeqNum(dynInstPtr);
    auto iter = this->preLSQ.find(seqNum);
    if (iter == preLSQ.end()) {
      // There is no special callbacks.
      INST_PANIC(dynInstPtr, "Only PreLSQ inst can check isAddrSizeReady.");
    }
    auto &callbacks = iter->second;
    for (auto &callback : callbacks) {
      if (callback) {
        Addr vaddr;
        uint32_t size;
        if (!callback->getAddrSize(vaddr, size)) {
          // This one is not ready yet.
          // INST_LOG(hack, dynInstPtr, "AddrSize not ready.\n");
          return false;
        }
      } else {
        // No more callbacks.
        break;
      }
    }
    return true;
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
  if (isGemForgeLoad) {
    assert(dynInstPtr->isMemRef() && "Should be MemRef for GemForgeLoad.");
    assert(dynInstPtr->isLoad() && "Should be Load for GemForgeLoad.");
    if (extraLQCallbacks.front()) {
      // There is at least one extra LQ callback.
      pimpl->preLSQ.emplace(std::piecewise_construct,
                            std::forward_as_tuple(dynInfo.seqNum),
                            std::forward_as_tuple(std::move(extraLQCallbacks)));
    } else {
      // This instruction is not considered as MemRef by GemForge.
      dynInstPtr->forceNotMemRef = true;
    }
  }
}

template <class CPUImpl>
bool DefaultO3CPUDelegator<CPUImpl>::canExecute(DynInstPtr &dynInstPtr) {
  /**
   * Special case for GemForgeLoad (inPreLSQ): they can execute when
   * addr/size is ready. Their GemForgeExecute hook is actually the
   * writeback event.
   */
  if (pimpl->isInPreLSQ(dynInstPtr)) {
    return pimpl->isAddrSizeReady(dynInstPtr);
  }
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
  if (pimpl->isInPreLSQ(dynInstPtr)) {
    // GemForgeLoad really happens at writeback.
    return;
  }
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  // DynInst is also ExecContext.
  pimpl->isaHandler.execute(dynInfo, *dynInstPtr);
}

template <class CPUImpl>
bool DefaultO3CPUDelegator<CPUImpl>::canWriteback(
    const DynInstPtr &dynInstPtr) {
  /**
   * Special case for GemForgeLoad: they can execute when
   * addr/size is ready. Their GemForgeExecute hook is actually the
   * writeback event.
   */
  if (dynInstPtr->isGemForge() && dynInstPtr->isLoad()) {
    auto dynInfo = pimpl->createDynInfo(dynInstPtr);
    auto ret = pimpl->isaHandler.canExecute(dynInfo);
    if (!ret) {
      INST_DPRINTF(dynInstPtr, "Cannot writeback.\n");
    }
    return ret;
  }
  return true;
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::writeback(const DynInstPtr &dynInstPtr) {
  if (dynInstPtr->isGemForge() && dynInstPtr->isLoad()) {
    INST_DPRINTF(dynInstPtr, "Writeback.\n");
    auto dynInfo = pimpl->createDynInfo(dynInstPtr);
    pimpl->isaHandler.execute(dynInfo, *dynInstPtr);
  }
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
  // All PreLSQ entries should already be cleared.
  if (pimpl->isInPreLSQ(dynInstPtr)) {
    INST_PANIC(dynInstPtr, "Still in PreLSQ when commit.");
  }
  pimpl->inflyInstQueue.pop_front();
  pimpl->isaHandler.commit(dynInfo);
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::squash(InstSeqNum squashSeqNum) {
  auto &inflyInstQueue = pimpl->inflyInstQueue;
  auto &preLSQ = pimpl->preLSQ;
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

    /**
     * Rewinding a instruction with GemForgeLQCallback involves 3 cases:
     *
     * 1. If the instruction is still in PreLSQ, we can simply discard it.
     *
     * 2. If the instruction is in the LSQ's request queue, it will be
     * marked complete in LSQ::tryToSendToTransfers() and moved to
     * transfers. Since it's already complete, it will eventually be
     * discarded in Execute::commit().
     *
     * 3. If the instruction is already in LSQ's transfer queue, normally
     * LSQ::findResponse() will call request->checkIsComplete(), which
     * eventually calls GemForgeLQCallback::isValueReady() and mark it
     * complete. After rewinding, we assume the callback is invalid to
     * use, therefore we explicitly mark the GemForgeLoadRequest discarded.
     *    We need explicitly marking it discarded because requests in
     * transfer queue is only marked complete when response comes back,
     * and Execute::commit() requires it to be completed before discarded.
     *
     * If the instruction is out of the transfer queue, it should already
     * be committed and not possible for it to be rewinded.
     */

    // 1. Release PreLSQ, if any.
    preLSQ.erase(seqNum);

    inflyInstQueue.pop_back();
  }
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::storeTo(Addr vaddr, int size) {
  // 1. Notify GemForge.
  pimpl->isaHandler.storeTo(vaddr, size);

  // 2. Find the oldest seqNum that aliased with this store.
  if (pimpl->inflyInstQueue.empty()) {
    return;
  }

  auto &preLSQ = pimpl->preLSQ;
  auto oldestMisspeculatedSeqNum =
      pimpl->getInstSeqNum(pimpl->inflyInstQueue.back());
  bool foundMisspeculated = false;

  auto checkMisspeculated =
      [vaddr, size, &foundMisspeculated, &oldestMisspeculatedSeqNum](
          InstSeqNum seqNum, GemForgeLQCallbackPtr &callback) -> void {
    if (!callback->isIssued()) {
      // This load is not issued yet, ignore it.
      return;
    }
    Addr ldVaddr;
    uint32_t ldSize;
    assert(callback->getAddrSize(ldVaddr, ldSize) &&
           "Issued LQCallback should have Addr/Size.");
    if (vaddr >= ldVaddr + ldSize || vaddr + size <= ldVaddr) {
      // No overlap.
      return;
    } else {
      // Aliased.
      if (callback->bypassAliasCheck()) {
        panic("Bypassed LQCallback is aliased: %s.\n", callback.get());
      }
      if (seqNum < oldestMisspeculatedSeqNum) {
        oldestMisspeculatedSeqNum = seqNum;
      }
      foundMisspeculated = true;
    }
  };

  // Check requests in preLSQ.
  for (auto &preLSQSeqNumRequest : preLSQ) {
    auto &seqNum = preLSQSeqNumRequest.first;
    for (auto &callback : preLSQSeqNumRequest.second) {
      if (callback) {
        checkMisspeculated(seqNum, callback);
      }
    }
  }

  // No misspeculation found in LSQ.
  if (!foundMisspeculated) {
    DPRINTF(O3CPUDelegator, "CPU storeTo %#x, %d, No Alias in PreLSQ.\n", vaddr,
            size);
    return;
  }

  DPRINTF(O3CPUDelegator, "CPU storeTo %#x, %d, Oldest Alias in PreLSQ %llu.\n",
          vaddr, size, oldestMisspeculatedSeqNum);
  // We have to mark all younger LQ callback as misspeculated.
  for (auto &preLSQSeqNumRequest : preLSQ) {
    auto &seqNum = preLSQSeqNumRequest.first;
    if (seqNum >= oldestMisspeculatedSeqNum) {
      for (auto &callback : preLSQSeqNumRequest.second) {
        if (callback && !callback->bypassAliasCheck()) {
          DPRINTF(O3CPUDelegator, "Flush PreLSQ AliasSeqNum %llu.\n", seqNum);
          callback->RAWMisspeculate();
        }
      }
    }
  }
}

template <class CPUImpl>
Fault DefaultO3CPUDelegator<CPUImpl>::initiateGemForgeLoad(
    const DynInstPtr &dynInstPtr) {
  assert(dynInstPtr->isGemForge() && dynInstPtr->isLoad() &&
         "Should be a GemForgeLoad.");
  auto &preLSQ = pimpl->preLSQ;
  auto seqNum = pimpl->getInstSeqNum(dynInstPtr);
  auto iter = preLSQ.find(seqNum);
  if (iter == preLSQ.end()) {
    // This is not GemForgeLoad
    INST_PANIC(dynInstPtr, "Missing PreLSQ for GemForgeLoad.");
  }

  auto &callbacks = iter->second;
  assert(callbacks.front() && "At least one GemForgeLQCallback.");
  // So far we only allow one LQCallback.
  assert(!callbacks.at(1) && "At most one GemForgeLQCallback.");

  Addr vaddr;
  uint32_t size;
  assert(callbacks.front()->getAddrSize(vaddr, size) &&
         "GemForgeLQCallback should be addr/size ready.");
  std::vector<bool> byteEnable;

  return pimpl->cpu->pushRequest(dynInstPtr, true /* isLoad */, nullptr, size,
                                 vaddr, 0 /* flags */, nullptr, nullptr,
                                 byteEnable);
}

template <class CPUImpl>
typename DefaultO3CPUDelegator<CPUImpl>::GFLoadReq *
DefaultO3CPUDelegator<CPUImpl>::allocateGemForgeLoadRequest(
    LSQUnit *lsq, const DynInstPtr &dynInstPtr) {
  auto &preLSQ = pimpl->preLSQ;
  auto seqNum = pimpl->getInstSeqNum(dynInstPtr);
  auto iter = preLSQ.find(seqNum);
  if (iter == preLSQ.end()) {
    // Not requiring GemForgeLoadRequest handling.
    return nullptr;
  }
  auto &callbacks = iter->second;
  assert(callbacks.front() && "At least one GemForgeLQCallback.");
  // So far we only allow one LQCallback.
  assert(!callbacks.at(1) && "At most one GemForgeLQCallback.");

  // Simply allocate the request.
  auto req = new GFLoadReq(lsq, dynInstPtr, this, std::move(callbacks.front()));

  // Release the preLSQ.
  preLSQ.erase(iter);
  return req;
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::discardGemForgeLoad(
    const DynInstPtr &dynInstPtr, GemForgeLQCallbackPtr callback) {
  assert(!dynInstPtr->isSquashed());
  INST_DPRINTF(dynInstPtr, "Back to PreLSQ.\n");
  auto &preLSQ = pimpl->preLSQ;
  auto seqNum = pimpl->getInstSeqNum(dynInstPtr);

  auto result =
      preLSQ.emplace(std::piecewise_construct, std::forward_as_tuple(seqNum),
                     std::forward_as_tuple());
  assert(result.second && "Callback already in PreLSQ.");
  result.first->second.front() = std::move(callback);
}

#undef INST_PANIC
#undef INST_DPRINTF

template class DefaultO3CPUDelegator<O3CPUImpl>;