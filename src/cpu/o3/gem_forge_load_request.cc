#include "gem_forge_load_request.hh"

#include "cpu/base.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/o3_cpu_delegator.hh"

#include "cpu/gem_forge/gem_forge_translation_fault.hh"
#include "debug/O3CPUDelegator.hh"

#define INST_DPRINTF(format, args...)                                          \
  DPRINTF(O3CPUDelegator, "[%s]: " format, *(this->_inst), ##args)
#define INST_PANIC(format, args...)                                            \
  panic("[%s]: " format, *(this->_inst), ##args)

namespace gem5 {

void GemForgeLoadRequest::release(Flag reason) {
  assert(reason == Flag::LSQEntryFreed || reason == Flag::Discarded);
  if (reason == Flag::LSQEntryFreed) {
    // Normal release.
    assert(!this->checkValueReadyEvent.scheduled());
    assert(!this->isAnyOutstandingRequest());
    delete this;
    return;
  }
  /**
   * Hard case: discard the request.
   * 1. Deschedule the checkValueReadyEvent.
   * 2. Discard is considered as misspeculation.
   * 3. Transfer the LQCallback back to CPUDelegator's PreLSQ.
   * 4. If there is a fake infly packet, we also create a fake reply so that
   *    this request get deleted immediately.
   */
  // 1. Deschedule event.
  if (this->checkValueReadyEvent.scheduled()) {
    this->cpuDelegator->deschedule(&this->checkValueReadyEvent);
  }
  // 2. Consider this as misspeculation.
  INST_DPRINTF(
      "GFLoadReq %s: Discard causes RAWMisspeculation on vaddr %#x size %d.\n",
      *this->callback, this->_addr, this->_size);
  this->callback->RAWMisspeculate();
  // 3. Push the callback back to PreLSQ.
  this->cpuDelegator->discardGemForgeLoad(this->instruction(),
                                          std::move(this->callback));
  // 4. Release myself.
  if (!this->isAnyOutstandingRequest()) {
    // Simple case: we can just delete it as before.
    delete this;
  } else {
    this->flags.set(reason);
    // Now let's fake the response.
    this->packetReplied();
  }
}

void GemForgeLoadRequest::initiateTranslation() {
  assert(this->_reqs.size() == 0);
  this->addReq(this->_addr, this->_size, this->_byteEnable);
  assert(this->_reqs.size() == 1 && "Should have request.");

  this->_reqs.back()->setReqInstSeqNum(this->_inst->seqNum);
  this->_reqs.back()->taskId(this->_taskId);
  this->_inst->translationStarted(true);
  this->setState(State::Translation);
  this->flags.set(Flag::TranslationStarted);
  this->_inst->savedRequest = this;

  // We do not initiate translation, as GemForge handles it.
  // Instead we immediately change the flag to finished.
  this->_fault.push_back(NoFault);
  this->numInTranslationFragments = 0;
  this->numTranslatedFragments = 1;
  if (this->_inst->isSquashed()) {
    this->squashTranslation();
  } else {
    Fault fault = NoFault;
    this->flags.set(Flag::TranslationFinished);
    // I get addr directly from CPUDelegator.
    Addr paddrLHS;
    Addr paddrRHS;
    if (!this->cpuDelegator->translateVAddrOracle(this->_addr, paddrLHS) ||
        !this->cpuDelegator->translateVAddrOracle(this->_addr + this->_size - 1,
                                                  paddrRHS)) {
      // There is translation fault.
      INST_DPRINTF("GFLoadReq %s: Translation fault on vaddr %#x size %d.\n",
                   *this->callback, this->_addr, this->_size);

      fault = std::make_shared<GemForge::GemForgeTranslationFault>();
      this->setState(State::Fault);
    } else {
      // No translation fault.
      this->_reqs.back()->setPaddr(paddrLHS);
      this->_inst->physEffAddr = this->_reqs.back()->getPaddr();
      this->_inst->memReqFlags = this->_reqs.back()->getFlags();
      this->setState(State::Request);
    }
    this->_inst->fault = fault;
    this->_inst->translationCompleted(true);
  }
}

void GemForgeLoadRequest::markAsStaleTranslation() {
  // GemForge do not translate,
  // so cannot have stale translations
  _hasStaleTranslation = false;
}

bool GemForgeLoadRequest::recvTimingResp(PacketPtr pkt) {
  // Never happens.
  panic("Not implemented.");
}

void GemForgeLoadRequest::buildPackets() {
  /* Retries do not create new packets. */
  if (this->_packets.size() == 0) {
    this->_packets.push_back(this->isLoad() ? Packet::createRead(this->req())
                                            : Packet::createWrite(this->req()));
    this->_packets.back()->dataStatic(this->_inst->memData);
    this->_packets.back()->senderState = this;
  }
  INST_DPRINTF("GFLoadReq: built packets.\n");
  assert(this->_packets.size() == 1);
}

void GemForgeLoadRequest::sendPacketToCache() {
  assert(this->_numOutstandingPackets == 0);
  /**
   * We don't really sent the packet, which is GemForge's job.
   * We just update my state to sent.
   */
  INST_DPRINTF("GFLoadReq: sendPacketToCache.\n");
  assert(!this->squashedInGemForge);
  assert(!this->rawMisspeculated);
  this->packetSent();
  this->_numOutstandingPackets = 1;
  /**
   * We schedule the value check for next cycle. Also we perform an immediate
   * value check to let the core know that I am waiting for the results.
   * This is used to remove the one-cycle delay from first value check to
   * issue the packet for non-floating AtomicCompute stream, which relies
   * on value check to know StreamAtomic is non-speculative.
   */
  this->callback->isValueReady();
  this->cpuDelegator->schedule(&this->checkValueReadyEvent, Cycles(1));
}

Cycles GemForgeLoadRequest::handleLocalAccess(ThreadContext *thread,
                                              PacketPtr pkt) {
  return pkt->req->localAccessor(thread, pkt);
}

bool GemForgeLoadRequest::isCacheBlockHit(Addr blockAddr, Addr blockMask) {
  return ((this->_reqs[0]->getPaddr() & blockMask) == blockAddr);
}

void GemForgeLoadRequest::checkValueReady() {
  assert(this->_numOutstandingPackets == 1);
  assert(!this->squashedInGemForge && "CheckValueReady after squashed.");
  assert(!this->rawMisspeculated && "CheckValueReady after RAWMisspeculation.");
  assert(!this->_inst->isSquashed());
  // Not squashed, check if value is ready.
  assert(this->callback && "No callback to checkValueReady.");
  // Check the LQ callback.
  bool completed = this->callback->isValueReady();
  INST_DPRINTF("GFLoadReq: %s.\n", completed ? "Ready" : "NotReady");
  if (!completed) {
    // Recheck next cycle.
    this->cpuDelegator->schedule(&this->checkValueReadyEvent, Cycles(1));
    return;
  } else {
    auto pkt = this->_packets.front();
    this->flags.set(Flag::Complete);
    // Remember to decrease _numOutstandingPackets here, otherwise myself
    // will not be released later.
    this->packetReplied();
    this->_port.completeDataAccess(pkt);
  }
}

bool GemForgeLoadRequest::hasOverlap(Addr vaddr, int size) const {
  Addr myVAddr;
  uint32_t mySize;
  if (!this->callback->getAddrSize(myVAddr, mySize)) {
    INST_PANIC("GFLoadReq: Failed to getAddrSize on LSQCallback: %s.\n",
               *this->callback);
  }
  if (myVAddr + mySize <= vaddr || myVAddr >= vaddr + size) {
    return false;
  }
  return true;
}

void GemForgeLoadRequest::squashInGemForge() {
  assert(!this->squashedInGemForge && "Already squashed in GemForge.");
  this->squashedInGemForge = true;
  if (this->isAnyOutstandingRequest()) {
    // 1. Deschedule check value ready event.
    assert(this->checkValueReadyEvent.scheduled());
    this->cpuDelegator->deschedule(&this->checkValueReadyEvent);
    // 2. Fake response so that later we will be released.
    this->packetReplied();
  }
}

void GemForgeLoadRequest::foundRAWMisspeculation() {
  if (this->rawMisspeculated) {
    INST_PANIC("GFLoadReq: Multiple RAWMisspeculation on LSQCallback: %s.\n",
               *this->callback);
  }
  INST_DPRINTF("GFLoadReq %s: Found RAWMisspeculation on vaddr %#x size %d.\n",
               *this->callback, this->_addr, this->_size);
  this->callback->RAWMisspeculate();
  this->rawMisspeculated = true;
  if (this->isAnyOutstandingRequest()) {
    // 1. Deschedule check value ready event.
    assert(this->checkValueReadyEvent.scheduled());
    this->cpuDelegator->deschedule(&this->checkValueReadyEvent);
    // 2. Fake response so that later we will be released.
    this->packetReplied();
  }
}

#undef INST_PANIC
#undef INST_DPRINTF

} // namespace gem5