#include "gem_forge_load_request.hh"
#include "cpu/o3/isa_specific.hh"

#include "cpu/gem_forge/gem_forge_translation_fault.hh"
#include "debug/O3CPUDelegator.hh"

#define INST_DPRINTF(format, args...)                                          \
  DPRINTF(O3CPUDelegator, "[%s]: " format, *(this->_inst), ##args)
#define INST_PANIC(format, args...)                                            \
  panic("[%s]: " format, *(this->_inst), ##args)

template <class Impl> void GemForgeLoadRequest<Impl>::release(Flag reason) {
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
  this->callback->RAWMisspeculate();
  // 3. Push the callback back to PreLSQ.
  this->cpuDelegator->discardGemForgeLoad(this->instruction(),
                                          std::move(this->callback));
  // 4. Release myself.
  if (!this->isAnyOutstandingRequest()) {
    // Simple case: we can just delete it as before.
    delete this;
  } else {
    assert(this->_senderState);
    this->_senderState->deleteRequest();
    this->flags.set(reason);
    // Now let's fake the response.
    this->_senderState->outstanding--;
    this->packetReplied();
  }
}

template <class Impl> void GemForgeLoadRequest<Impl>::initiateTranslation() {
  assert(this->_requests.size() == 0);
  this->addRequest(this->_addr, this->_size, this->_byteEnable);
  assert(this->_requests.size() == 1 && "Should have request.");

  this->_requests.back()->setReqInstSeqNum(this->_inst->seqNum);
  this->_requests.back()->taskId(this->_taskId);
  this->_inst->translationStarted(true);
  this->setState(State::Translation);
  this->flags.set(Flag::TranslationStarted);
  this->_inst->savedReq = this;

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
      INST_DPRINTF("GFLoadReq: Translation fault on vaddr %#x size %d.\n",
                   this->_addr, this->_size);

      fault = std::make_shared<GemForge::GemForgeLoadTranslationFault>();
      this->setState(State::Fault);
    } else {
      // No translation fault.
      this->_requests.back()->setPaddr(paddrLHS);
      this->_inst->physEffAddr = this->_requests.back()->getPaddr();
      this->_inst->memReqFlags = this->_requests.back()->getFlags();
      this->setState(State::Request);
    }
    this->_inst->fault = fault;
    this->_inst->translationCompleted(true);
  }
}

template <class Impl>
bool GemForgeLoadRequest<Impl>::recvTimingResp(PacketPtr pkt) {
  // Never happens.
  panic("Not implemented.");
}

template <class Impl> void GemForgeLoadRequest<Impl>::buildPackets() {
  assert(this->_senderState);
  /* Retries do not create new packets. */
  if (this->_packets.size() == 0) {
    this->_packets.push_back(this->isLoad()
                                 ? Packet::createRead(this->request())
                                 : Packet::createWrite(this->request()));
    this->_packets.back()->dataStatic(this->_inst->memData);
    this->_packets.back()->senderState = this->_senderState;
  }
  INST_DPRINTF("GFLoadReq: built packets.\n");
  assert(this->_packets.size() == 1);
}

template <class Impl> void GemForgeLoadRequest<Impl>::sendPacketToCache() {
  assert(this->_numOutstandingPackets == 0);
  /**
   * We don't really sent the packet, which is GemForge's job.
   * We just update my state to sent.
   */
  INST_DPRINTF("GFLoadReq: sendPacketToCache.\n");
  assert(!this->squashedInGemForge);
  assert(!this->rawMisspeculated);
  auto *packet = this->_packets.at(0);
  auto state = dynamic_cast<LSQSenderState *>(packet->senderState);
  state->outstanding++;
  this->packetSent();
  this->_numOutstandingPackets = 1;
  // Schedule value check.
  this->cpuDelegator->schedule(&this->checkValueReadyEvent, Cycles(1));
}

template <class Impl>
Cycles GemForgeLoadRequest<Impl>::handleLocalAccess(ThreadContext *thread,
                                                    PacketPtr pkt) {
  return pkt->req->localAccessor(thread, pkt);
}

template <class Impl>
bool GemForgeLoadRequest<Impl>::isCacheBlockHit(Addr blockAddr,
                                                Addr blockMask) {
  return ((this->_requests[0]->getPaddr() & blockMask) == blockAddr);
}

template <class Impl> void GemForgeLoadRequest<Impl>::checkValueReady() {
  assert(this->_numOutstandingPackets == 1);
  assert(!this->squashedInGemForge && "CheckValueReady after squashed.");
  assert(!this->rawMisspeculated && "CheckValueReady after RAWMisspeculation.");
  assert(!this->_inst->isSquashed());
  // Not squashed, check if value is ready.
  assert(this->callback && "No callback to checkValueReady.");
  // Check the LQ callback.
  bool completed = this->callback->isValueLoaded();
  INST_DPRINTF("GFLoadReq: %s.\n", completed ? "Ready" : "NotReady");
  if (!completed) {
    // Recheck next cycle.
    this->cpuDelegator->schedule(&this->checkValueReadyEvent, Cycles(1));
    return;
  } else {
    auto pkt = this->_packets.front();
    auto state = dynamic_cast<LSQSenderState *>(pkt->senderState);
    this->flags.set(Flag::Complete);
    state->outstanding--;
    // Remember to decrease _numOutstandingPackets here, otherwise myself
    // will not be released later.
    this->packetReplied();
    this->_port.completeDataAccess(pkt);
  }
}

template <class Impl>
bool GemForgeLoadRequest<Impl>::hasOverlap(Addr vaddr, int size) const {
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

template <class Impl> void GemForgeLoadRequest<Impl>::squashInGemForge() {
  assert(!this->squashedInGemForge && "Already squashed in GemForge.");
  this->squashedInGemForge = true;
  if (this->isAnyOutstandingRequest()) {
    // 1. Deschedule check value ready event.
    assert(this->checkValueReadyEvent.scheduled());
    this->cpuDelegator->deschedule(&this->checkValueReadyEvent);
    // 2. Fake response so that later we will be released.
    assert(this->_senderState);
    this->_senderState->deleteRequest();
    this->_senderState->outstanding--;
    this->packetReplied();
  }
}

template <class Impl> void GemForgeLoadRequest<Impl>::foundRAWMisspeculation() {
  if (this->rawMisspeculated) {
    INST_PANIC("GFLoadReq: Multiple RAWMisspeculation on LSQCallback: %s.\n",
               *this->callback);
  }
  this->callback->RAWMisspeculate();
  this->rawMisspeculated = true;
  if (this->isAnyOutstandingRequest()) {
    // 1. Deschedule check value ready event.
    assert(this->checkValueReadyEvent.scheduled());
    this->cpuDelegator->deschedule(&this->checkValueReadyEvent);
    // 2. Fake response so that later we will be released.
    assert(this->_senderState);
    this->_senderState->deleteRequest();
    this->_senderState->outstanding--;
    this->packetReplied();
  }
}

#undef INST_PANIC
#undef INST_DPRINTF

// Force instantiate the class.
template class GemForgeLoadRequest<O3CPUImpl>;