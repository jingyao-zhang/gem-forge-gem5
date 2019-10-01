#include "minor_cpu_delegator.hh"

#include "gem_forge_load_request.hh"

#include "pipeline.hh"

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
      : cpu(_cpu), cpuDelegator(_cpuDelegator), isaHandler(_cpuDelegator),
        drainPendingPacketsEvent(
            [this]() -> void { this->cpuDelegator->drainPendingPackets(); },
            _cpu->name()) {}

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
   * Store the LQ callbacks before the they are really inserted into
   * the LSQ after FU. There is nor order between instructions in the PreLSQ.
   */
  std::unordered_map<InstSeqNum, GemForgeLQCallbackList> preLSQ;

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

  EventFunctionWrapper drainPendingPacketsEvent;
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
  GemForgeLQCallbackList extraLQCallbacks;
  pimpl->isaHandler.dispatch(dynInfo, extraLQCallbacks);
  pimpl->inflyInstQueue.push_back(dynInstPtr);
  // TODO: Insert these extra LQCallbacks into PreLSQ.
  if (extraLQCallbacks.front()) {
    // There are at least one extra LQ callbacks.
    pimpl->preLSQ.emplace(std::piecewise_construct,
                          std::forward_as_tuple(dynInstPtr->id.execSeqNum),
                          std::forward_as_tuple(std::move(extraLQCallbacks)));
  }
}

bool MinorCPUDelegator::isAddrSizeReady(Minor::MinorDynInstPtr &dynInstPtr) {
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
        return false;
      }
    } else {
      // No more callbacks.
      break;
    }
  }
  return true;
}

void MinorCPUDelegator::insertLSQ(Minor::MinorDynInstPtr &dynInstPtr) {
  auto &preLSQ = pimpl->preLSQ;
  auto seqNum = dynInstPtr->id.execSeqNum;
  auto iter = preLSQ.find(seqNum);
  // INST_LOG(hack, dynInstPtr, "Insert into LSQ.\n");
  if (iter == preLSQ.end()) {
    // Not our special instruction requires LSQ handling.
    return;
  }
  /**
   * So far the LSQ requires the address ready, we enforce that.
   */
  auto &callbacks = iter->second;
  assert(callbacks.front() && "At least one LQ callback.");
  auto &pipeline = pimpl->cpu->pipeline;
  auto &lsq = pipeline->execute.getLSQ();
  for (auto &callback : callbacks) {
    if (!callback) {
      // We reached the end of the callbacks.
      break;
    }
    assert(lsq.canRequest() && "LSQ full for GemForge inst.");
    // Get the address and size.
    Addr vaddr;
    uint32_t size;
    // TODO: Delay inserting into the LSQ when the address is not ready.
    assert(callback->getAddrSize(vaddr, size) &&
           "The addr/size is not ready yet.");

    // This basically means that one Inst can generate only one LSQ entry.
    assert(!dynInstPtr->inLSQ && "Inst already in LSQ.");

    INST_DPRINTF(dynInstPtr, "Insert into LSQ (%#x, %u).\n", vaddr, size);
    Minor::LSQ::LSQRequestPtr request =
        new Minor::GemForgeLoadRequest(lsq, dynInstPtr, std::move(callback));

    // Have to setup the request.
    int cid = pimpl->getThreadContext(dynInstPtr)->contextId();
    request->request->setContext(cid);
    request->request->setVirt(0 /* asid */, vaddr, size, 0 /* flags */,
                              baseCPU->dataMasterId(),
                              dynInstPtr->pc.instAddr());
    // No ByteEnable.

    // Create the packet.
    request->makePacket();

    // Insert the special GemForgeLoadRequest.
    dynInstPtr->inLSQ = true;
    lsq.requests.push(request);
    request->startAddrTranslation();
  }
  /**
   * Clear the preLSQ as they are now in LSQ.
   */
  preLSQ.erase(seqNum);
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
  // All PreLSQ entries should already be cleared.
  assert(pimpl->preLSQ.count(dynInstPtr->id.execSeqNum) == 0 &&
         "PreLSQ entries found when commit.");
  pimpl->inflyInstQueue.pop_front();
  pimpl->isaHandler.commit(dynInfo);
}

void MinorCPUDelegator::streamChange(InstSeqNum newStreamSeqNum) {
  // Rewind the inflyInstQueue.
  auto &inflyInstQueue = pimpl->inflyInstQueue;
  auto &preLSQ = pimpl->preLSQ;
  while (!inflyInstQueue.empty() &&
         inflyInstQueue.back()->id.streamSeqNum != newStreamSeqNum) {
    // This needs to be rewind.
    auto &misspeculatedInst = inflyInstQueue.back();
    auto dynInfo = pimpl->createDynInfo(misspeculatedInst);
    pimpl->isaHandler.rewind(dynInfo);
    // Release the misspeculated LQ callback.
    preLSQ.erase(misspeculatedInst->id.execSeqNum);
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
  // If this is not a load request, we should send immediately.
  // e.g. StreamConfit/End packet.
  if (pkt->cmd != MemCmd::ReadReq) {
    auto &lsq = pimpl->cpu->pipeline->execute.getLSQ();
    assert(lsq.dcachePort->sendTimingReqVirtual(pkt));
    return;
  }

  pimpl->pendingPkts.push_back(pkt);
  if (!pimpl->drainPendingPacketsEvent.scheduled()) {
    this->drainPendingPackets();
  }
}

namespace {
/**
 * A fake LSQRequest, used to conform with StoreBuffer::canForwardDataToLoad().
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
      assert(lsq.dcachePort->sendTimingReqVirtual(pkt));
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
