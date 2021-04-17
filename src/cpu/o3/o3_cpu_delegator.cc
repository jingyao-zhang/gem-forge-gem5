#include "o3_cpu_delegator.hh"

#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/impl.hh"
#include "debug/O3CPUDelegator.hh"
#include "debug/O3CPUDelegatorDump.hh"
#include "debug/StreamAlias.hh"

#define INST_DPRINTF(inst, format, args...)                                    \
  DPRINTF(O3CPUDelegator, "[%s]: " format, *(inst), ##args)
#define INST_PANIC(inst, format, args...)                                      \
  panic("[%s]: " format, *(inst), ##args)

template <class CPUImpl> class DefaultO3CPUDelegator<CPUImpl>::Impl {
public:
  using GFLoadReq = typename DefaultO3CPUDelegator<CPUImpl>::GFLoadReq;

  Impl(O3CPU *_cpu, DefaultO3CPUDelegator *_cpuDelegator)
      : cpu(_cpu), cpuDelegator(_cpuDelegator) {}

  O3CPU *cpu;
  DefaultO3CPUDelegator<CPUImpl> *cpuDelegator;

  // Cache of the traceExtraFolder.
  std::string traceExtraFolder;

  /**
   * For simplicity, we maintain our own queue of infly instruction.
   */
  std::deque<DynInstPtr> inflyInstQueue;

  /**
   * Remember if the inst has been squashed in GemForge.
   * Due to ROB squashWidth, the instruction itself may not been marked
   * as squashed yet.
   */
  std::map<InstSeqNum, bool> inflyInstSquashedMap;

  /**
   * Store the LQ callbacks before they are really marked vaddr ready
   * and tracked by the LSQ.
   */
  std::unordered_map<InstSeqNum, GemForgeLSQCallbackList> preLSQ;

  /**
   * Store the LQ requests that are tracked by the LSQ.
   * This is used for squashing.
   */
  std::unordered_map<InstSeqNum, GFLoadReq *> inLSQ;

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

  bool isSquashedInGemForge(const DynInstPtr &dynInstPtr) const {
    auto seqNum = this->getInstSeqNum(dynInstPtr);
    auto iter = this->inflyInstSquashedMap.find(seqNum);
    if (iter == this->inflyInstSquashedMap.end()) {
      INST_PANIC(dynInstPtr, "Missed in InflyInstSquashedMap.");
    }
    return iter->second;
  }

  void squashInGemForge(const DynInstPtr &dynInstPtr) {
    auto seqNum = this->getInstSeqNum(dynInstPtr);
    auto iter = this->inflyInstSquashedMap.find(seqNum);
    if (iter == this->inflyInstSquashedMap.end()) {
      INST_PANIC(dynInstPtr, "Missed in InflyInstSquashedMap.");
    }
    if (iter->second) {
      INST_PANIC(dynInstPtr, "Already SquashedInGemForge.");
    }
    iter->second = true;
  }

  void releaseInflyInstSquashedMap(InstSeqNum committedSeqNum) {
    // Release all entries that are older than (<=) committedSeqNum.
    auto upperBound = this->inflyInstSquashedMap.upper_bound(committedSeqNum);
    this->inflyInstSquashedMap.erase(this->inflyInstSquashedMap.begin(),
                                     upperBound);
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

  bool isLoadInPreLSQ(const DynInstPtr &dynInstPtr) {
    auto seqNum = this->getInstSeqNum(dynInstPtr);
    return this->preLSQ.count(seqNum) &&
           this->preLSQ.at(seqNum).front()->getType() ==
               GemForgeLSQCallback::Type::LOAD;
  }

  bool isGemForgeLoadOrAtomic(const DynInstPtr &dynInstPtr) {
    return dynInstPtr->isGemForge() &&
           (dynInstPtr->isLoad() || dynInstPtr->isAtomic());
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
        if (!callback->isAddrReady()) {
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

template <class CPUImpl> void DefaultO3CPUDelegator<CPUImpl>::regStats() {
#define scalar(stat, describe)                                                 \
  this->stat.name(pimpl->cpu->name() + ("." #stat))                            \
      .desc(describe)                                                          \
      .prereq(this->stat)

  scalar(statCoreDataHops, "Accumulated core data hops.");
  scalar(statCoreDataHopsIgnored, "Accumulated core data hops ignored.");
  scalar(statCoreCachedDataHops, "Accumulated core data hops with cache.");
  scalar(statCoreCachedDataHopsIgnored,
         "Accumulated core data hops ignored wit cache.");
  scalar(statCoreCommitMicroOps, "Accumulated core committed micro ops.");
  scalar(statCoreCommitMicroOpsIgnored,
         "Accumulated core committed micro ops ignored.");
  scalar(statCoreCommitMicroOpsGemForge,
         "Accumulated core committed GemForge micro ops.");
}

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
      return pTable->translate(vaddr, paddr);
    }
    return false;
  }
  return true;
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::sendRequest(PacketPtr pkt) {
  auto lineBytes = this->cacheLineSize();
  if ((pkt->getAddr() % lineBytes) + pkt->getSize() > lineBytes) {
    panic("Multi-line packet paddr %#x size %d.", pkt->getAddr(),
          pkt->getSize());
  }
  auto &lsq = pimpl->cpu->getIEW()->ldstQueue;
  assert(lsq.getDataPortPtr()->sendTimingReqVirtual(pkt, false /* isCore */));
}

template <class CPUImpl>
bool DefaultO3CPUDelegator<CPUImpl>::canDispatch(DynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  auto ret = isaHandler->canDispatch(dynInfo);
  if (!ret) {
    INST_DPRINTF(dynInstPtr, "Cannot dispatch.\n");
  }
  return ret;
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::dispatch(DynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  INST_DPRINTF(dynInstPtr, "Dispatch.\n");
  GemForgeLSQCallbackList extraLSQCallbacks;
  isaHandler->dispatch(dynInfo, extraLSQCallbacks);
  pimpl->inflyInstQueue.push_back(dynInstPtr);
  pimpl->inflyInstSquashedMap.emplace(dynInfo.seqNum, false);
  if (extraLSQCallbacks.front()) {
    // There is at least one extra LSQ callback.
    INST_DPRINTF(dynInstPtr, "Dispatch with extra LSQCallback.\n");
    pimpl->preLSQ.emplace(std::piecewise_construct,
                          std::forward_as_tuple(dynInfo.seqNum),
                          std::forward_as_tuple(std::move(extraLSQCallbacks)));
  } else if (dynInstPtr->staticInst->isGemForge() &&
             dynInstPtr->staticInst->isMemRef()) {
    // This instruction is not considered as MemRef by GemForge.
    dynInstPtr->forceNotMemRef = true;
  }
}

template <class CPUImpl>
bool DefaultO3CPUDelegator<CPUImpl>::canExecute(DynInstPtr &dynInstPtr) {
  /**
   * Special case for GemForgeLoad/Atomic (inPreLSQ):
   * Can execute when addr/size is ready. Their GemForgeExecute hook is actually
   * the writeback event.
   * Notice that GemForgeAtomic also uses LoadCallback.
   */
  if (pimpl->isSquashedInGemForge(dynInstPtr)) {
    // Already squashed, not exposed to GemForge.
    return true;
  }
  if (pimpl->isLoadInPreLSQ(dynInstPtr)) {
    return pimpl->isAddrSizeReady(dynInstPtr);
  }
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  auto ret = isaHandler->canExecute(dynInfo);
  if (!ret) {
    INST_DPRINTF(dynInstPtr, "Cannot execute.\n");
  }
  return ret;
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::execute(DynInstPtr &dynInstPtr) {
  if (pimpl->isSquashedInGemForge(dynInstPtr)) {
    // Already squashed, not exposed to GemForge.
    return;
  }
  INST_DPRINTF(dynInstPtr, "Execute.\n");
  if (pimpl->isLoadInPreLSQ(dynInstPtr)) {
    // GemForgeLoad/Atomic really happens at writeback.
    return;
  }
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  // DynInst is also ExecContext.
  isaHandler->execute(dynInfo, *dynInstPtr);
}

template <class CPUImpl>
bool DefaultO3CPUDelegator<CPUImpl>::canWriteback(
    const DynInstPtr &dynInstPtr) {
  if (pimpl->isSquashedInGemForge(dynInstPtr)) {
    // Already squashed, not exposed to GemForge.
    return true;
  }
  if (pimpl->isGemForgeLoadOrAtomic(dynInstPtr)) {
    auto dynInfo = pimpl->createDynInfo(dynInstPtr);
    auto ret = isaHandler->canExecute(dynInfo);
    if (!ret) {
      INST_DPRINTF(dynInstPtr, "Cannot writeback.\n");
    }
    return ret;
  }
  return true;
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::writeback(const DynInstPtr &dynInstPtr) {
  if (pimpl->isSquashedInGemForge(dynInstPtr)) {
    // Already squashed, not exposed to GemForge.
    return;
  }
  if (pimpl->isGemForgeLoadOrAtomic(dynInstPtr)) {
    INST_DPRINTF(dynInstPtr, "Writeback.\n");
    auto dynInfo = pimpl->createDynInfo(dynInstPtr);
    isaHandler->execute(dynInfo, *dynInstPtr);
  }
}

template <class CPUImpl>
bool DefaultO3CPUDelegator<CPUImpl>::canCommit(const DynInstPtr &dynInstPtr) {
  if (pimpl->isSquashedInGemForge(dynInstPtr)) {
    // Already squashed, not exposed to GemForge.
    return true;
  }
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  auto ret = isaHandler->canCommit(dynInfo);
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
  // All PreLSQ entries should already be cleared, except for GemForgeStore.
  if (pimpl->isInPreLSQ(dynInstPtr)) {
    auto iter = pimpl->preLSQ.find(dynInfo.seqNum);
    if (iter->second.front()->getType() == GemForgeLSQCallback::Type::STORE) {
      pimpl->preLSQ.erase(iter);
    } else {
      INST_PANIC(dynInstPtr, "Still in PreLSQ when commit.");
    }
  }

  auto seqNum = pimpl->getInstSeqNum(dynInstPtr);
  if (pimpl->isSquashedInGemForge(dynInstPtr)) {
    // Already squashed, not exposed to GemForge.
    INST_PANIC(dynInstPtr, "Commit SquashedInGemForge inst.");
  }

  // Record committed stats.
  this->statCoreCommitMicroOps++;
  if (this->dataTrafficAcc.shouldIgnoreTraffic(dynInstPtr->pcState().pc())) {
    this->statCoreCommitMicroOpsIgnored++;
  }
  if (dynInstPtr->staticInst->isGemForge()) {
    this->statCoreCommitMicroOpsGemForge++;
  }

  // Clear InLSQ.
  pimpl->inLSQ.erase(seqNum);

  pimpl->inflyInstQueue.pop_front();
  pimpl->releaseInflyInstSquashedMap(seqNum);
  isaHandler->commit(dynInfo);
}

template <class CPUImpl>
bool DefaultO3CPUDelegator<CPUImpl>::shouldCountInPipeline(
    const DynInstPtr &dynInstPtr) {
  if (dynInstPtr->isSquashed()) {
    return true;
  }
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  return isaHandler->shouldCountInPipeline(dynInfo);
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::squash(InstSeqNum squashSeqNum) {
  auto &inflyInstQueue = pimpl->inflyInstQueue;
  auto &preLSQ = pimpl->preLSQ;
  auto &inLSQ = pimpl->inLSQ;
  while (!inflyInstQueue.empty()) {
    auto &inst = inflyInstQueue.back();
    auto seqNum = pimpl->getInstSeqNum(inst);
    if (seqNum <= squashSeqNum) {
      // We are done.
      break;
    }
    INST_DPRINTF(inst, "Rewind.\n");
    auto dynInfo = pimpl->createDynInfo(inst);
    isaHandler->rewind(dynInfo);

    /**
     * Rewinding a instruction with GemForgeLSQCallback involves 3 cases:
     *
     * 1. If the instruction is still in PreLSQ, we can simply discard it.
     * 2. If the instruction is already in LSQ, we mark it squashed.
     */

    // 1. Release PreLSQ, if any.
    preLSQ.erase(seqNum);

    // 2. Mark InLSQ squshed.
    auto inLSQIter = inLSQ.find(seqNum);
    if (inLSQIter != inLSQ.end()) {
      inLSQIter->second->squashInGemForge();
      inLSQ.erase(inLSQIter);
    }

    inflyInstQueue.pop_back();
    // Remember the squash decision.
    pimpl->squashInGemForge(inst);
  }
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::storeTo(InstSeqNum seqNum, Addr vaddr,
                                             int size) {
  // 1. Notify GemForge.
  isaHandler->storeTo(seqNum, vaddr, size);

  // 2. Find the oldest seqNum that aliased with this store.
  if (pimpl->inflyInstQueue.empty()) {
    return;
  }

  auto &preLSQ = pimpl->preLSQ;
  auto oldestMisspeculatedSeqNum =
      pimpl->getInstSeqNum(pimpl->inflyInstQueue.back());
  bool foundMisspeculated = false;
  bool misspeculatedHasNonCoreDep = false;

  auto checkMisspeculated =
      [vaddr, size, &foundMisspeculated, &misspeculatedHasNonCoreDep,
       &oldestMisspeculatedSeqNum](InstSeqNum seqNum,
                                   GemForgeLSQCallbackPtr &callback) -> void {
    if (!callback->isIssued()) {
      // This load is not issued yet, ignore it.
      return;
    }
    Addr ldVaddr;
    uint32_t ldSize;
    assert(callback->getAddrSize(ldVaddr, ldSize) &&
           "Issued LSQCallback should have Addr/Size.");
    if (vaddr >= ldVaddr + ldSize || vaddr + size <= ldVaddr) {
      // No overlap.
      return;
    } else {
      // Aliased.
      if (callback->bypassAliasCheck()) {
        panic("Bypassed LSQCallback is aliased: %s.\n", callback.get());
      }
      if (seqNum < oldestMisspeculatedSeqNum) {
        oldestMisspeculatedSeqNum = seqNum;
      }
      foundMisspeculated = true;
      if (callback->hasNonCoreDependent()) {
        misspeculatedHasNonCoreDep = true;
      }
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

  // No misspeculation found in PreLSQ.
  if (!foundMisspeculated) {
    DPRINTF(O3CPUDelegator, "CPU storeTo %#x, %d, No Alias in PreLSQ.\n", vaddr,
            size);
    return;
  }

  DPRINTF(
      O3CPUDelegator,
      "CPU storeTo %#x, %d, Oldest Alias in PreLSQ %llu, HasNonCoreDep %d.\n",
      vaddr, size, oldestMisspeculatedSeqNum, misspeculatedHasNonCoreDep);
  // We have to mark all younger LQ callback as misspeculated.
  for (auto &preLSQSeqNumRequest : preLSQ) {
    auto &seqNum = preLSQSeqNumRequest.first;
    if (seqNum >= oldestMisspeculatedSeqNum) {
      for (auto &callback : preLSQSeqNumRequest.second) {
        if (callback && callback->isIssued() && !callback->bypassAliasCheck()) {
          Addr ldVaddr;
          uint32_t ldSize;
          assert(callback->getAddrSize(ldVaddr, ldSize) &&
                 "Issued LSQCallback should have Addr/Size.");
          bool aliased =
              !(vaddr >= ldVaddr + ldSize || vaddr + size <= ldVaddr);
          if (!aliased && !misspeculatedHasNonCoreDep) {
            // This is not aliased, and misspeculation has no non-core dep.
            // We can perform selective flush.
            DPRINTF(O3CPUDelegator, "Skip Flush PreLSQ AliasSeqNum %llu.\n",
                    seqNum);
            continue;
          }
          DPRINTF(O3CPUDelegator, "Flush PreLSQ AliasSeqNum %llu.\n", seqNum);
          callback->RAWMisspeculate();
        }
      }
    }
  }
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::foundRAWMisspeculationInLSQ(
    InstSeqNum checkSeqNum, Addr vaddr, int size) {
  // Check for all callbacks in LSQ that is >= aliasSeqNum.
  if (pimpl->inflyInstQueue.empty()) {
    return;
  }

  // There should only be a few InLSQ callbacks, so I directly search in it.
  bool foundMisspeculated = false;
  bool misspeculatedHasNonCoreDep = false;
  InstSeqNum oldestAliasSeqNum = 0;
  for (const auto &inLSQEntry : pimpl->inLSQ) {
    auto seqNum = inLSQEntry.first;
    auto entry = inLSQEntry.second;
    if (seqNum > checkSeqNum && !entry->bypassAliasCheck() &&
        !entry->hasRAWMisspeculated()) {
      if (entry->hasOverlap(vaddr, size)) {
        if (!foundMisspeculated) {
          // First time.
          oldestAliasSeqNum = seqNum;
          foundMisspeculated = true;
        } else {
          oldestAliasSeqNum = std::min(oldestAliasSeqNum, seqNum);
        }
        if (inLSQEntry.second->hasNonCoreDependent()) {
          misspeculatedHasNonCoreDep = true;
        }
      }
    }
  }
  if (!foundMisspeculated) {
    return;
  }
  DPRINTF(O3CPUDelegator,
          "Found RAWMisspeculation in LSQ: CheckSeq %llu OldestAliasSeq %llu "
          "HasNonCoreUser %d.\n",
          checkSeqNum, oldestAliasSeqNum, misspeculatedHasNonCoreDep);
  // Flush those after oldestAliasSeqNum.
  for (const auto &inLSQEntry : pimpl->inLSQ) {
    auto seqNum = inLSQEntry.first;
    auto entry = inLSQEntry.second;
    if (seqNum >= oldestAliasSeqNum && !entry->bypassAliasCheck() &&
        !entry->hasRAWMisspeculated()) {
      bool aliased = entry->hasOverlap(vaddr, size);
      if (!aliased && !misspeculatedHasNonCoreDep) {
        // Selectively flush.
        continue;
      }
      DPRINTF(O3CPUDelegator, "Flush InLSQ AliasSeqNum %llu.\n", seqNum);
      entry->foundRAWMisspeculation();
    }
  }
}

template <class CPUImpl>
Fault DefaultO3CPUDelegator<CPUImpl>::initiateGemForgeLoadOrAtomic(
    const DynInstPtr &dynInstPtr) {
  assert(pimpl->isGemForgeLoadOrAtomic(dynInstPtr) &&
         "Should be a GemForgeLoad/Atomic.");
  auto &preLSQ = pimpl->preLSQ;
  auto seqNum = pimpl->getInstSeqNum(dynInstPtr);
  auto iter = preLSQ.find(seqNum);
  if (iter == preLSQ.end()) {
    // This is not GemForgeLoad
    INST_PANIC(dynInstPtr, "Missing PreLSQ for GemForgeLoad/Atomic.");
  }

  auto &callbacks = iter->second;
  assert(callbacks.front() && "At least one GemForgeLQCallback.");
  // So far we only allow one LSQCallback.
  assert(!callbacks.at(1) && "At most one GemForgeLQCallback.");

  Addr vaddr;
  uint32_t size;
  assert(callbacks.front()->getAddrSize(vaddr, size) &&
         "GemForgeLQCallback should be addr/size ready.");
  std::vector<bool> byteEnable;

  return pimpl->cpu->pushRequest(dynInstPtr, dynInstPtr->isLoad() /* isLoad */,
                                 nullptr /* data */, size, vaddr, 0 /* flags */,
                                 nullptr, nullptr, byteEnable);
}

template <class CPUImpl>
Fault DefaultO3CPUDelegator<CPUImpl>::initiateGemForgeStore(
    const DynInstPtr &dynInstPtr) {
  assert(dynInstPtr->isGemForge() && dynInstPtr->isStore() &&
         "Should be a GemForgeStore.");
  auto &preLSQ = pimpl->preLSQ;
  auto seqNum = pimpl->getInstSeqNum(dynInstPtr);
  auto iter = preLSQ.find(seqNum);
  if (iter == preLSQ.end()) {
    // This is not GemForgeLoad
    INST_PANIC(dynInstPtr, "Missing PreLSQ for GemForgeStore.");
  }

  auto &callbacks = iter->second;
  assert(callbacks.front() && "At least one GemForgeSQCallback.");
  // So far we only allow one LSQCallback.
  assert(!callbacks.at(1) && "At most one GemForgeSQCallback.");

  Addr vaddr;
  uint32_t size;
  assert(callbacks.front()->getAddrSize(vaddr, size) &&
         "GemForgeSQCallback should be addr/size ready.");
  assert(callbacks.front()->isValueReady() &&
         "GemForgeSQCallback should be value ready.");

  const uint8_t *constValue = callbacks.front()->getValue();
  /**
   * ! GemForge provides const pointer, while O3 cpu wants original one.
   */
  uint8_t *storeValue = const_cast<uint8_t *>(constValue);

  std::vector<bool> byteEnable;

  return pimpl->cpu->pushRequest(dynInstPtr, false /* isLoad */,
                                 storeValue /* data */, size, vaddr,
                                 0 /* flags */, nullptr, nullptr, byteEnable);
}

template <class CPUImpl>
typename DefaultO3CPUDelegator<CPUImpl>::GFLoadReq *
DefaultO3CPUDelegator<CPUImpl>::allocateGemForgeLoadRequest(
    LSQUnit *lsq, const DynInstPtr &dynInstPtr) {
  auto &preLSQ = pimpl->preLSQ;
  auto &inLSQ = pimpl->inLSQ;
  auto seqNum = pimpl->getInstSeqNum(dynInstPtr);
  auto iter = preLSQ.find(seqNum);
  if (iter == preLSQ.end()) {
    // Not requiring GemForgeLoadRequest handling.
    return nullptr;
  }
  auto &callbacks = iter->second;
  assert(callbacks.front() && "At least one GemForgeLSQCallback.");
  // So far we only allow one LSQCallback.
  assert(!callbacks.at(1) && "At most one GemForgeLSQCallback.");

  if (callbacks.front()->getType() == GemForgeLSQCallback::Type::LOAD) {
    // Push load into inLSQ
    INST_DPRINTF(dynInstPtr, "GFLoadReq %s Release in PreLSQ.\n",
                 *callbacks.front());
    auto req =
        new GFLoadReq(lsq, dynInstPtr, this, std::move(callbacks.front()));
    auto ret =
        inLSQ.emplace(std::piecewise_construct, std::forward_as_tuple(seqNum),
                      std::forward_as_tuple(req));
    assert(ret.second && "Already inLSQ.");

    // Release the preLSQ.
    preLSQ.erase(iter);
    return req;
  } else {
    // For GemForgeStore, we simply do not release the callback in case the
    // core deferred the request and we would lose the callback.
    // Now in the core, it is just a normal store request. If there is memory
    // order misspeculation, the core should rewind it and restart again.
    return nullptr;
  }
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::discardGemForgeLoad(
    const DynInstPtr &dynInstPtr, GemForgeLSQCallbackPtr callback) {
  assert(!dynInstPtr->isSquashed());
  INST_DPRINTF(dynInstPtr, "GFReq %s Back to PreLSQ.\n", *callback);
  auto &preLSQ = pimpl->preLSQ;
  auto &inLSQ = pimpl->inLSQ;
  auto seqNum = pimpl->getInstSeqNum(dynInstPtr);

  auto result =
      preLSQ.emplace(std::piecewise_construct, std::forward_as_tuple(seqNum),
                     std::forward_as_tuple());
  assert(result.second && "Callback already in PreLSQ.");
  result.first->second.front() = std::move(callback);

  // Erase from inLSQ.
  auto inLSQIter = inLSQ.find(seqNum);
  assert(inLSQIter != inLSQ.end() && "Missed inLSQ.");
  inLSQ.erase(inLSQIter);
}

template <class CPUImpl>
InstSeqNum DefaultO3CPUDelegator<CPUImpl>::getInstSeqNum() const {
  return pimpl->cpu->getGlobalInstSeq();
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::setInstSeqNum(InstSeqNum seqNum) {
  pimpl->cpu->setGlobalInstSeq(seqNum);
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::recordCoreDataTraffic(
    const DynInstPtr &dynInstPtr, Addr vaddr, int size) {
  if (dynInstPtr->staticInst->isGemForge()) {
    // Ignore GemForgeLoad/Store.
    return;
  }

  Addr paddr;
  if (!this->translateVAddrOracle(vaddr, paddr)) {
    // Ignore fault.
    return;
  }

  if (!this->ideaCache) {
    // No idea cache. Do not bother.
    return;
  }

  int myBank = this->cpuId();
  int dataBank = CoreDataTrafficAccumulator::mapPAddrToBank(paddr);
  int distance = CoreDataTrafficAccumulator::getDistance(myBank, dataBank);
  int flits = CoreDataTrafficAccumulator::getNumFlits(size);
  int missSize = this->ideaCache->access(paddr, size);
  int missFlits = CoreDataTrafficAccumulator::getNumFlits(missSize);
  this->statCoreDataHops += distance * flits;
  this->statCoreCachedDataHops += distance * missFlits;
  if (this->dataTrafficAcc.shouldIgnoreTraffic(dynInstPtr->pcState().pc())) {
    this->statCoreDataHopsIgnored += distance * flits;
    this->statCoreCachedDataHopsIgnored += distance * missFlits;
  }
}

template <class CPUImpl>
std::vector<std::string>
    DefaultO3CPUDelegator<CPUImpl>::CoreDataTrafficAccumulator::ignoredFuncs{
        // "__kmp_hyper_barrier_release",
        // "__kmp_hardware_timestamp",
        // "__kmp_join_barrier",
        // "__kmp_barrier_template",
        // "__kmp_fork_call",
        "__kmp_",
        "__kmpc_",
    };

template <class CPUImpl>
bool DefaultO3CPUDelegator<
    CPUImpl>::CoreDataTrafficAccumulator::shouldIgnoreTraffic(Addr pc) {
  auto iter = this->pcIgnoredMap.find(pc);
  if (iter == this->pcIgnoredMap.end()) {
    bool ignore = false;
    Addr funcStart = 0;
    Addr funcEnd = 0;
    std::string symbol;
    bool found = Loader::debugSymbolTable->findNearestSymbol(
        pc, symbol, funcStart, funcEnd);
    if (!found) {
      symbol = csprintf("0x%x", pc);
    }
    for (const auto &f : ignoredFuncs) {
      auto pos = symbol.find(f);
      if (pos != std::string::npos) {
        ignore = true;
        break;
      }
    }
    iter = this->pcIgnoredMap.emplace(pc, ignore).first;
  }
  return iter->second;
}

template <class CPUImpl>
void DefaultO3CPUDelegator<CPUImpl>::recordStatsForFakeExecutedInst(
    const StaticInstPtr &inst) {
  pimpl->cpu->decode.decodeDecodedInsts++;
  pimpl->cpu->rename.renameRenamedOperands += inst->numDestRegs();
  pimpl->cpu->rename.renameRenameLookups += inst->numSrcRegs();
  pimpl->cpu->rob.robReads++;
  pimpl->cpu->rob.robWrites++;
  pimpl->cpu->commit.opsCommitted[0]++;
  if (inst->isInteger()) {
    pimpl->cpu->commit.statComInteger[0]++;
    pimpl->cpu->iew.instQueue.intAluAccesses++;
    pimpl->cpu->intRegfileReads += inst->numSrcRegs();
    pimpl->cpu->intRegfileWrites += inst->numDestRegs();
  }
  if (inst->isFloating()) {
    pimpl->cpu->commit.statComFloating[0]++;
    pimpl->cpu->iew.instQueue.fpAluAccesses++;
    pimpl->cpu->fpRegfileReads += inst->numSrcRegs();
    pimpl->cpu->fpRegfileWrites += inst->numDestRegs();
  }
  if (inst->isVector()) {
    pimpl->cpu->commit.statComVector[0]++;
    pimpl->cpu->iew.instQueue.vecAluAccesses++;
    pimpl->cpu->vecRegfileReads += inst->numSrcRegs();
    pimpl->cpu->vecRegfileWrites += inst->numDestRegs();
  }
}

#undef INST_PANIC
#undef INST_DPRINTF

template class DefaultO3CPUDelegator<O3CPUImpl>;