#include "minor_cpu_delegator.hh"

#include "gem_forge_load_request.hh"

#include "pipeline.hh"

#include "debug/MinorCPUDelegator.hh"
#include "debug/MinorCPUDelegatorDump.hh"

#include "cpu/gem_forge/gem_forge_translation_fault.hh"

#define INST_DPRINTF(inst, format, args...)                                    \
  DPRINTF(MinorCPUDelegator, "[%s]: " format, *(inst), ##args)
#define INST_LOG(log, inst, format, args...)                                   \
  log("[%s]: " format, *(inst), ##args)
#define INST_PANIC(impl, inst, format, args...)                                \
  impl->dumpInflyInsts();                                                      \
  panic("[%s]: " format, *(inst), ##args)

class MinorCPUDelegator::Impl {
public:
  Impl(MinorCPU *_cpu, MinorCPUDelegator *_cpuDelegator)
      : cpu(_cpu), cpuDelegator(_cpuDelegator),
        drainPendingPacketsEvent(
            [this]() -> void { this->cpuDelegator->drainPendingPackets(); },
            _cpu->name()),
        dumpInflyInstsEvent(
            [this]() -> void {
              if (!Debug::MinorCPUDelegatorDump) {
                return;
              }
              this->dumpInflyInsts();
              this->cpuDelegator->schedule(&this->dumpInflyInstsEvent,
                                           Cycles(100000));
            },
            _cpu->name()) {}

  MinorCPU *cpu;
  MinorCPUDelegator *cpuDelegator;

  // Cache of the traceExtraFolder.
  std::string traceExtraFolder;

  /**
   * For simplicity, we maintain our own queue of infly instruction.
   */
  std::deque<Minor::MinorDynInstPtr> inflyInstQueue;

  /**
   * Store the LQ callbacks before the they are really inserted into
   * the LSQ after FU. There is nor order between instructions in the PreLSQ.
   */
  std::unordered_map<InstSeqNum, GemForgeLSQCallbackList> preLSQ;

  /**
   * Stores the GemForgeLoadRequest in the LSQ, after the callback from preLSQ
   * inserted into the LSQ.
   */
  std::unordered_map<InstSeqNum, std::vector<Minor::GemForgeLoadRequest *>>
      inLSQ;

  /**
   * Stores the packets pending to be sent.
   */
  std::deque<PacketPtr> pendingPkts;

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

  GemForgeDynInstInfo createDynInfo(Minor::MinorDynInstPtr &dynInstPtr) const {
    if (!dynInstPtr->isInst()) {
      panic("Should be a real inst, but a fault %d.\n", dynInstPtr->isFault());
    }
    if (dynInstPtr->id.streamSeqNum != this->currentStreamSeqNum) {
      INST_PANIC(this, dynInstPtr, "Mismatched streamSeqNum %llu.\n",
                 this->currentStreamSeqNum);
    }
    GemForgeDynInstInfo dynInfo(this->getInstSeqNum(dynInstPtr), dynInstPtr->pc,
                                dynInstPtr->staticInst.get(),
                                this->getThreadContext(dynInstPtr));
    return dynInfo;
  }

  void dumpInflyInsts() const {
    hack("========= MinorCPUDelegatorDump ===========\n");
    for (const auto &dynInstPtr : this->inflyInstQueue) {
      hack("%s\n", *dynInstPtr);
    }
    hack("======= MinorCPUDelegatorDump End =========\n");
  }

  EventFunctionWrapper drainPendingPacketsEvent;
  EventFunctionWrapper dumpInflyInstsEvent;
};

/**********************************************************************
 * MinorCPUDelegator.
 *********************************************************************/

MinorCPUDelegator::MinorCPUDelegator(MinorCPU *_cpu)
    : GemForgeCPUDelegator(CPUTypeE::MINOR, _cpu), pimpl(new Impl(_cpu, this)) {
}
MinorCPUDelegator::~MinorCPUDelegator() = default;

void MinorCPUDelegator::startup() {
  this->schedule(&pimpl->dumpInflyInstsEvent, Cycles(1));
}

bool MinorCPUDelegator::shouldCountInPipeline(
    Minor::MinorDynInstPtr &dynInstPtr) {
  if (!dynInstPtr->isInst()) {
    // This is not handled by me, should always count.
    return true;
  }
  // Checking with the isaHandler.
  // At this stage, there is no valid sequence number, so we can't use
  // pimpl->createDynInfo().
  GemForgeDynInstInfo dynInfo(0, dynInstPtr->pc, dynInstPtr->staticInst.get(),
                              pimpl->getThreadContext(dynInstPtr));
  return isaHandler->shouldCountInPipeline(dynInfo);
}

bool MinorCPUDelegator::canDispatch(Minor::MinorDynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  auto ret = isaHandler->canDispatch(dynInfo);
  if (!ret) {
    INST_DPRINTF(dynInstPtr, "Cannot dispatch.\n");
  }
  return ret;
}

void MinorCPUDelegator::dispatch(Minor::MinorDynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  INST_DPRINTF(dynInstPtr, "Dispatch.\n");
  GemForgeLSQCallbackList extraLSQCallbacks;
  isaHandler->dispatch(dynInfo, extraLSQCallbacks);
  pimpl->inflyInstQueue.push_back(dynInstPtr);
  if (extraLSQCallbacks.front()) {
    // There are at least one extra LQ callbacks.
    pimpl->preLSQ.emplace(std::piecewise_construct,
                          std::forward_as_tuple(dynInstPtr->id.execSeqNum),
                          std::forward_as_tuple(std::move(extraLSQCallbacks)));
  } else if (dynInstPtr->staticInst->isGemForge() &&
             dynInstPtr->staticInst->isMemRef()) {
    /**
     * ! Pure Evil
     * GemForge allows different behaviors of the same static instruction at
     * runtime. Here, a GemForgeLoad without extraLQCallbacks is not considered
     * to be a load anymore, i.e. it is treated as a "move" from SE to core.
     * An example of this is a dynamic StreamLoad and not the first user of
     * that StreamElement.
     *
     * We do this by setting forceNotMemRef in the dynamic instruction.
     */
    dynInstPtr->forceNotMemRef = true;
  }
}

bool MinorCPUDelegator::canInsertLSQ(Minor::MinorDynInstPtr &dynInstPtr) {
  auto &preLSQ = pimpl->preLSQ;
  auto seqNum = dynInstPtr->id.execSeqNum;
  auto iter = preLSQ.find(seqNum);
  if (iter == preLSQ.end()) {
    // There is no special callbacks.
    return true;
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
      // Check ValueReady for StoreCallback.
      if (callback->getType() == GemForgeLSQCallback::Type::STORE) {
        if (!callback->isValueReady()) {
          return false;
        }
      }
    } else {
      // No more callbacks.
      break;
    }
  }
  return true;
}

Fault MinorCPUDelegator::insertLSQ(Minor::MinorDynInstPtr &dynInstPtr) {
  auto &preLSQ = pimpl->preLSQ;
  auto &inLSQ = pimpl->inLSQ;
  auto seqNum = dynInstPtr->id.execSeqNum;
  auto iter = preLSQ.find(seqNum);
  // INST_LOG(hack, dynInstPtr, "Insert into LSQ.\n");
  if (iter == preLSQ.end()) {
    // Not our special instruction requires LSQ handling.
    return NoFault;
  }
  /**
   * So far the LSQ requires the address ready, we enforce that.
   */
  auto &callbacks = iter->second;
  assert(callbacks.front() && "At least one LQ callback.");
  auto &pipeline = pimpl->cpu->pipeline;
  auto &lsq = pipeline->execute.getLSQ();

  assert(callbacks.front() && "At least one GemForgeLQCallback.");
  // So far we only allow one LSQCallback.
  assert(!callbacks.at(1) && "At most one GemForgeLQCallback.");

  auto &callback = callbacks.front();
  // Get the address and size.
  Addr vaddr;
  uint32_t size;
  // TODO: Delay inserting into the LSQ when the address is not ready.
  assert(callback->getAddrSize(vaddr, size) &&
         "The addr/size is not ready yet.");

  INST_DPRINTF(dynInstPtr, "Insert into LSQ (%#x, %u).\n", vaddr, size);

  assert(!dynInstPtr->inLSQ && "Inst already in LSQ.");
  assert(lsq.canRequest() && "LSQ full for GemForge inst.");
  if (callback->getType() == GemForgeLSQCallback::Type::LOAD) {
    // This basically means that one Inst can generate only one LSQ entry.

    /**
     * LQCallback is handled as special request.
     */
    GemForgeLQCallbackPtr lqCallback = nullptr;
    lqCallback.reset(static_cast<GemForgeLQCallback *>(callback.release()));
    auto request =
        new Minor::GemForgeLoadRequest(lsq, dynInstPtr, std::move(lqCallback));

    // Have to setup the request.
    int cid = pimpl->getThreadContext(dynInstPtr)->contextId();
    request->request->setContext(cid);
    request->request->setVirt(vaddr, size, 0 /* flags */,
                              baseCPU->dataMasterId(),
                              dynInstPtr->pc.instAddr());

    /**
     * The StoreBuffer requires the physical address for store-to-load check.
     * We hack there to set the physical address for the place holder request.
     */
    Addr paddrLHS;
    Addr paddrRHS;
    if (!this->translateVAddrOracle(vaddr, paddrLHS) ||
        !this->translateVAddrOracle(vaddr + size - 1, paddrRHS)) {
      // There is translation fault.
      INST_DPRINTF(dynInstPtr, "Translation fault on vaddr %#x size %d.\n",
                   vaddr, size);
      if (dynInstPtr->translationFault == NoFault) {
        dynInstPtr->translationFault =
            std::make_shared<GemForge::GemForgeTranslationFault>();
      }
      /**
       * Request the state of the request to Translated.
       * When sent to transfer queue, LSQ will mark it completed and skipped.
       * If it ever gets to commit stage, our translation fault will be invoked
       * and we will get a panic.
       */
      request->setState(Minor::LSQ::LSQRequest::LSQRequestState::Translated);
    } else {
      request->request->setPaddr(paddrLHS);
      // Create the packet.
      request->makePacket();
    }

    // No ByteEnable.

    // Insert the special GemForgeLoadRequest.

    /**
     * Push takes Minor::LSQ::LSQRequestPtr&, which requires a lvalue so
     * we have to do the type cast by ourselves.
     */
    {
      Minor::LSQ::LSQRequestPtr lsqRequest = request;
      lsq.requests.push(lsqRequest);
    }
    // Push into inLSQ.
    inLSQ
        .emplace(std::piecewise_construct, std::forward_as_tuple(seqNum),
                 std::forward_as_tuple())
        .first->second.push_back(request);

    request->startAddrTranslation();

    /**
     * Clear the preLSQ as they are now in LSQ.
     */
    preLSQ.erase(seqNum);
    dynInstPtr->inLSQ = true;

    return dynInstPtr->translationFault;

  } else {
    /**
     * SQCallback is handled as a normal store request.
     * And it is not released from preLSQ until committed.
     */
    assert(callback->isValueReady() &&
           "GemForgeSQCallback should be value ready.");
    const uint8_t *constValue = callback->getValue();
    /**
     * ! GemForge provides const pointer, while Minor LSQ wants original one.
     */
    uint8_t *storeValue = const_cast<uint8_t *>(constValue);

    auto fault =
        lsq.pushRequest(dynInstPtr, false /* isLoad */, storeValue, size, vaddr,
                        0 /* flags */, nullptr /* res */, nullptr /* amo_op */);
    INST_DPRINTF(dynInstPtr, "Insert into LSQ. Fault %s.\n", fault);
    return fault;
  }
}

InstSeqNum MinorCPUDelegator::getEarlyIssueMustWaitSeqNum(
    Minor::MinorDynInstPtr &dynInstPtr) {
  // This should make this function no effect.
  return 0;
}

bool MinorCPUDelegator::canExecute(Minor::MinorDynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  auto ret = isaHandler->canExecute(dynInfo);
  if (!ret) {
    INST_DPRINTF(dynInstPtr, "Cannot execute.\n");
  }
  return ret;
}

void MinorCPUDelegator::execute(Minor::MinorDynInstPtr &dynInstPtr,
                                ExecContext &xc) {
  INST_DPRINTF(dynInstPtr, "Execute.\n");
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  isaHandler->execute(dynInfo, xc);
}

bool MinorCPUDelegator::canCommit(Minor::MinorDynInstPtr &dynInstPtr) {
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  auto ret = isaHandler->canCommit(dynInfo);
  if (!ret) {
    INST_DPRINTF(dynInstPtr, "Cannot commit.\n");
  }
  return ret;
}

void MinorCPUDelegator::commit(Minor::MinorDynInstPtr &dynInstPtr) {
  INST_DPRINTF(dynInstPtr, "Commit.\n");
  auto dynInfo = pimpl->createDynInfo(dynInstPtr);
  assert(!pimpl->inflyInstQueue.empty() &&
         "Empty inflyInstQueue to commit from.");

  // Notify the idea inorder cpu.
  // TODO: Do not update the opc too frequently.
  if (this->ideaInorderCPU) {
    this->ideaInorderCPU->addOp(dynInfo);
    this->ideaInorderCPUNoFUTiming->addOp(dynInfo);
    this->ideaInorderCPUNoLDTiming->addOp(dynInfo);
    pimpl->cpu->stats.ideaCycles = this->ideaInorderCPU->getCycles();
    pimpl->cpu->stats.ideaCyclesNoFUTiming =
        this->ideaInorderCPUNoFUTiming->getCycles();
    pimpl->cpu->stats.ideaCyclesNoLDTiming =
        this->ideaInorderCPUNoLDTiming->getCycles();
  }

  auto &frontInst = pimpl->inflyInstQueue.front();
  if (frontInst != dynInstPtr) {
    INST_LOG(panic, dynInstPtr, "Commit mismatch inflyInstQueue front %s.",
             *frontInst);
  }
  // All PreLSQ entries should already be cleared, except for GemForgeStore.
  if (pimpl->preLSQ.count(dynInstPtr->id.execSeqNum)) {
    auto iter = pimpl->preLSQ.find(dynInfo.seqNum);
    if (iter->second.front()->getType() == GemForgeLSQCallback::Type::STORE) {
      pimpl->preLSQ.erase(iter);
    } else {
      INST_PANIC(pimpl, dynInstPtr, "Still in PreLSQ when commit.");
    }
  }

  /**
   * Release InLSQ entries, if any.
   * ! These request is already released in LSQ::popResponse().
   */
  pimpl->inLSQ.erase(dynInstPtr->id.execSeqNum);
  pimpl->inflyInstQueue.pop_front();
  isaHandler->commit(dynInfo);
}

void MinorCPUDelegator::streamChange(InstSeqNum newStreamSeqNum) {
  // Rewind the inflyInstQueue.
  auto &inflyInstQueue = pimpl->inflyInstQueue;
  auto &preLSQ = pimpl->preLSQ;
  auto &inLSQ = pimpl->inLSQ;
  // if (newStreamSeqNum == 22152) {
  //   hack("Dump due to streamChange to %llu.\n", newStreamSeqNum);
  //   pimpl->dumpInflyInsts();
  // }
  while (!inflyInstQueue.empty() &&
         inflyInstQueue.back()->id.streamSeqNum != newStreamSeqNum) {
    // This needs to be rewind.
    auto &misspeculatedInst = inflyInstQueue.back();
    INST_DPRINTF(misspeculatedInst, "Rewind.\n");
    auto dynInfo = pimpl->createDynInfo(misspeculatedInst);
    isaHandler->rewind(dynInfo);

    /**
     * Rewinding a instruction with GemForgeLQCallback involves 3 cases:
     *
     * 1. If the instruction is still in FU (not inserted into LSQ), the
     * callback is still in preLSQ. Erase it from preLSQ and it will be
     * discarded when exit the FU.
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
    preLSQ.erase(misspeculatedInst->id.execSeqNum);

    // 2. Mark InLSQ GemForgeLoadRequest discarded, if any.
    auto inLSQIter = inLSQ.find(misspeculatedInst->id.execSeqNum);
    if (inLSQIter != inLSQ.end()) {
      for (auto loadRequest : inLSQIter->second) {
        loadRequest->markDiscarded();
      }
      // Erase inLSQ.
      inLSQ.erase(inLSQIter);
    }

    // Pop from the inflyInstQueue.
    inflyInstQueue.pop_back();
  }

  /**
   * Is there any instruction remaining?
   */
  // if (!inflyInstQueue.empty()) {
  //   pimpl->dumpInflyInsts();
  // }

  pimpl->currentStreamSeqNum = newStreamSeqNum;
}

void MinorCPUDelegator::storeTo(InstSeqNum seqNum, Addr vaddr, int size) {
  // First notify the IsaHandler.
  isaHandler->storeTo(seqNum, vaddr, size);
  // Find the oldest seqNum that aliased with this store.
  if (pimpl->inflyInstQueue.empty()) {
    // This should actually never happen.
    return;
  }
  auto &preLSQ = pimpl->preLSQ;
  auto &inLSQ = pimpl->inLSQ;
  // * This relies on that execSeqNum is never decreasing.
  auto oldestMisspeculatedSeqNum = pimpl->inflyInstQueue.back()->id.execSeqNum;
  bool foundMisspeculated = false;

  auto checkMisspeculated =
      [vaddr, size, &foundMisspeculated, &oldestMisspeculatedSeqNum](
          InstSeqNum seqNum, GemForgeLSQCallback *callback) -> void {
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
        panic("Bypassed LQCallback is aliased: %s.\n", *callback);
      }
      if (seqNum < oldestMisspeculatedSeqNum) {
        oldestMisspeculatedSeqNum = seqNum;
      }
      foundMisspeculated = true;
    }
  };

  // Check request within inLSQ.
  for (auto &inLSQSeqNumRequest : inLSQ) {
    auto &seqNum = inLSQSeqNumRequest.first;
    for (auto &request : inLSQSeqNumRequest.second) {
      checkMisspeculated(seqNum, request->callback.get());
    }
  }

  // Check requests in preLSQ.
  for (auto &preLSQSeqNumRequest : preLSQ) {
    auto &seqNum = preLSQSeqNumRequest.first;
    for (auto &callback : preLSQSeqNumRequest.second) {
      if (callback && callback->getType() == GemForgeLSQCallback::Type::LOAD) {
        checkMisspeculated(seqNum, callback.get());
      }
    }
  }

  // No misspeculation found in LSQ.
  if (!foundMisspeculated) {
    return;
  }

  // For all younger LQ callback, we make it misspeculated.
  for (auto &inLSQSeqNumRequest : inLSQ) {
    auto &seqNum = inLSQSeqNumRequest.first;
    if (seqNum >= oldestMisspeculatedSeqNum) {
      for (auto &request : inLSQSeqNumRequest.second) {
        if (!request->callback->bypassAliasCheck()) {
          request->callback->RAWMisspeculate();
        }
      }
    }
  }

  for (auto &preLSQSeqNumRequest : preLSQ) {
    auto &seqNum = preLSQSeqNumRequest.first;
    if (seqNum >= oldestMisspeculatedSeqNum) {
      for (auto &callback : preLSQSeqNumRequest.second) {
        if (callback && !callback->bypassAliasCheck()) {
          callback->RAWMisspeculate();
        }
      }
    }
  }
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

bool MinorCPUDelegator::translateVAddrOracle(Addr vaddr, Addr &paddr) {
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

void MinorCPUDelegator::sendRequest(PacketPtr pkt) {
  // If this is not a load request, we should send immediately.
  // e.g. StreamConfig/End packet.
  if (pkt->cmd != MemCmd::ReadReq) {
    auto &lsq = pimpl->cpu->pipeline->execute.getLSQ();
    assert(lsq.dcachePort->sendTimingReqVirtual(pkt, false /* isCore */));
    return;
  }
  auto lineBytes = this->cacheLineSize();
  if ((pkt->getAddr() % lineBytes) + pkt->getSize() > lineBytes) {
    panic("Multi-line packet paddr %#x size %d.", pkt->getAddr(),
          pkt->getSize());
  }

  pimpl->pendingPkts.push_back(pkt);
  if (!pimpl->drainPendingPacketsEvent.scheduled()) {
    this->drainPendingPackets();
  }
}

namespace {
/**
 * A fake LSQRequest, used to conform with
 * StoreBuffer::canForwardDataToLoad().
 */
class FakeLoadRequest : public Minor::LSQ::LSQRequest {
protected:
  void finish(const Fault &fault, const RequestPtr &request, ThreadContext *tc,
              BaseTLB::Mode mode) override {
    panic("%s not implemented.", __PRETTY_FUNCTION__);
  }

public:
  void startAddrTranslation() override {
    panic("%s not implemented.", __PRETTY_FUNCTION__);
  }
  PacketPtr getHeadPacket() override {
    panic("%s not implemented.", __PRETTY_FUNCTION__);
  }
  void stepToNextPacket() override {
    panic("%s not implemented.", __PRETTY_FUNCTION__);
  }
  bool hasPacketsInMemSystem() override {
    panic("%s not implemented.", __PRETTY_FUNCTION__);
  }
  bool sentAllPackets() override {
    panic("%s not implemented.", __PRETTY_FUNCTION__);
  }
  void retireResponse(PacketPtr packet) override {
    panic("%s not implemented.", __PRETTY_FUNCTION__);
  }

  FakeLoadRequest(Minor::LSQ &_port, Minor::MinorDynInstPtr _inst,
                  RequestPtr _request)
      : LSQRequest(_port, _inst, true /* isLoad */) {
    this->request = _request;
  }
};
} // namespace

void MinorCPUDelegator::drainPendingPackets() {
  /**
   * This handles the request from GemForge.
   *
   * 1. If there is any aliased store in the StoreBuffer, blocked
   * until the store buffer is drained empty. There should be a
   * GemForgeLQCallback in the LSQ to block any future store to be
   * inserted into the StoreBuffer, therefore the memory order is
   * maintained.
   *
   * TODO: Correctly handle the memory barrier.
   */

  auto &lsq = pimpl->cpu->pipeline->execute.getLSQ();
  auto &storeBuffer = lsq.storeBuffer;
  auto &pendingPkts = pimpl->pendingPkts;
  while (!pendingPkts.empty()) {
    auto &pkt = pendingPkts.front();
    /**
     * Create the fake LSQRequest for the storeBuffer. It needs:
     * 1. The threadId. Since no SMT, the threaId should always be 0.
     * 2. The request for the physical address and size.
     */

    // We have to use RefCountingPtr to avoid memory leak.
    Minor::MinorDynInstPtr fakeDynInst(new Minor::MinorDynInst());
    fakeDynInst->id.threadId = 0;
    FakeLoadRequest fakeLSQRequest(lsq, fakeDynInst, pkt->req);

    unsigned int forwardSlot;
    auto addrRange =
        storeBuffer.canForwardDataToLoad(&fakeLSQRequest, forwardSlot);
    bool issued = false;
    switch (addrRange) {
    case Minor::LSQ::AddrRangeCoverage::NoAddrRangeCoverage: {
      // This packet can be sent to dcache port.
      assert(lsq.dcachePort->sendTimingReqVirtual(pkt, false /* isCore */));
      issued = true;
      break;
    }
    case Minor::LSQ::AddrRangeCoverage::FullAddrRangeCoverage:
    case Minor::LSQ::AddrRangeCoverage::PartialAddrRangeCoverage: {
      // For far we will wait until there is no alised store.
      issued = false;
      break;
    }
    }

    if (issued) {
      pendingPkts.pop_front();
    } else {
      break;
    }
  }

  if (!pendingPkts.empty()) {
    // Reschedule for next cycle.
    this->schedule(&pimpl->drainPendingPacketsEvent, Cycles(1));
  }
}

InstSeqNum MinorCPUDelegator::getInstSeqNum() const {
  // Make sure this is not SMT.
  assert(pimpl->cpu->numThreads == 1 && "GemForge doest not support SMT.");
  return pimpl->cpu->pipeline->decode.getFirstThreadExecSeqNum();
}

void MinorCPUDelegator::setInstSeqNum(InstSeqNum seqNum) {
  // Make sure this is not SMT.
  assert(pimpl->cpu->numThreads == 1 && "GemForge doest not support SMT.");
  pimpl->cpu->pipeline->decode.setFirstThreadExecSeqNum(seqNum);
}

void MinorCPUDelegator::recordStatsForFakeExecutedInst(
    const StaticInstPtr &inst) {
  pimpl->cpu->stats.numDecodedOps++;
  pimpl->cpu->stats.numOps++;

  if (inst->isInteger()) {
    pimpl->cpu->stats.numCommittedIntOps++;
    pimpl->cpu->stats.numIQIntReads++;
    pimpl->cpu->stats.numIQIntWrites++;
    pimpl->cpu->stats.numIntRegReads += inst->numSrcRegs();
    pimpl->cpu->stats.numIntRegWrites += inst->numDestRegs();
  }
  if (inst->isFloating() || inst->isVector()) {
    pimpl->cpu->stats.numCommittedFpOps++;
    pimpl->cpu->stats.numIQFpReads++;
    pimpl->cpu->stats.numIQFpWrites++;
    pimpl->cpu->stats.numFpRegReads += inst->numSrcRegs();
    pimpl->cpu->stats.numFpRegWrites += inst->numDestRegs();
  }
}