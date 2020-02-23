
#include "LLCStreamEngine.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

// Generated by slicc.
#include "mem/ruby/protocol/StreamMigrateRequestMsg.hh"
#include "mem/simple_mem.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "base/trace.hh"
#include "debug/LLCRubyStream.hh"
#define DEBUG_TYPE LLCRubyStream
#include "../stream_log.hh"

#define LLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(LLCRubyStream, "[LLC_SE%d]: " format,                                \
          this->controller->getMachineID().num, ##args)

LLCStreamEngine::LLCStreamEngine(AbstractStreamAwareController *_controller,
                                 MessageBuffer *_streamMigrateMsgBuffer,
                                 MessageBuffer *_streamIssueMsgBuffer,
                                 MessageBuffer *_streamIndirectIssueMsgBuffer,
                                 MessageBuffer *_streamResponseMsgBuffer)
    : Consumer(_controller), controller(_controller),
      streamMigrateMsgBuffer(_streamMigrateMsgBuffer),
      streamIssueMsgBuffer(_streamIssueMsgBuffer),
      streamIndirectIssueMsgBuffer(_streamIndirectIssueMsgBuffer),
      streamResponseMsgBuffer(_streamResponseMsgBuffer), issueWidth(1),
      migrateWidth(1), maxInflyRequests(8), maxInqueueRequests(2) {}

LLCStreamEngine::~LLCStreamEngine() {
  for (auto &s : this->streams) {
    delete s;
    s = nullptr;
  }
  this->streams.clear();
}

void LLCStreamEngine::receiveStreamConfigure(PacketPtr pkt) {
  auto streamConfigureData = *(pkt->getPtr<CacheStreamConfigureData *>());
  LLCSE_DPRINTF("Received Pkt %#x, StreamConfigure %#x, initVAddr "
                "%#x, "
                "initPAddr %#x.\n",
                pkt, streamConfigureData, streamConfigureData->initVAddr,
                streamConfigureData->initPAddr);
  std::unordered_map<DynamicStreamId, LLCDynamicStream *, DynamicStreamIdHasher>
      configuredStreamMap;

  // Create the stream.
  auto S = new LLCDynamicStream(this->controller, streamConfigureData);
  configuredStreamMap.emplace(S->getDynamicStreamId(), S);

  // Check if we have indirect streams.
  for (auto &ISConfig : streamConfigureData->indirectStreams) {
    // Let's create an indirect stream.
    ISConfig->initAllocatedIdx = streamConfigureData->initAllocatedIdx;
    auto IS = new LLCDynamicStream(this->controller, ISConfig.get());
    configuredStreamMap.emplace(IS->getDynamicStreamId(), IS);
    LLC_S_DPRINTF(IS->getDynamicStreamId(),
                  "Configure IndirectStream size %d, config size %d.\n",
                  IS->getElementSize(), ISConfig->elementSize);
    S->indirectStreams.push_back(IS);
    IS->baseStream = S;
  }

  // Create predicated stream information.
  assert(!streamConfigureData->isPredicated &&
         "Base stream should never be predicated.");
  for (auto IS : S->indirectStreams) {
    if (IS->isPredicated()) {
      const auto &predSId = IS->getPredicateStreamId();
      assert(configuredStreamMap.count(predSId) != 0 &&
             "Failed to find predicate stream.");
      auto predS = configuredStreamMap.at(predSId);
      assert(predS != IS && "Self predication.");
      predS->predicatedStreams.insert(IS);
      IS->predicateStream = predS;
    }
  }

  this->streams.emplace_back(S);
  // Release memory.
  delete streamConfigureData;
  delete pkt;

  // Let's schedule a wakeup event.
  this->scheduleEvent(Cycles(1));
}

void LLCStreamEngine::receiveStreamEnd(PacketPtr pkt) {
  auto endStreamDynamicId = *(pkt->getPtr<DynamicStreamId *>());
  LLC_S_DPRINTF(*endStreamDynamicId, "Received StreamEnd.\n");
  // Search for this stream.
  for (auto streamIter = this->streams.begin(), streamEnd = this->streams.end();
       streamIter != streamEnd; ++streamIter) {
    auto &stream = *streamIter;
    if (stream->getDynamicStreamId() == (*endStreamDynamicId)) {
      // Found it.
      // ? Can we just sliently release it?
      LLC_S_DPRINTF(*endStreamDynamicId, "Ended.\n");
      delete stream;
      stream = nullptr;
      this->streams.erase(streamIter);
      return;
    }
  }
  /**
   * ? No need to search in migratingStreams?
   * For migrating streams, the end message should be sent to the destination
   * llcBank.
   */

  /**
   * If not found, it is similar case as stream flow control message.
   * We are waiting for the stream to migrate here.
   * Add the message to the pending
   */
  this->pendingStreamEndMsgs.insert(*endStreamDynamicId);

  // Don't forgot to release the memory.
  delete endStreamDynamicId;
  delete pkt;
}

void LLCStreamEngine::receiveStreamMigrate(LLCDynamicStreamPtr stream) {

  // Sanity check.
  Addr vaddr = stream->peekVAddr();
  Addr paddr;
  assert(stream->translateToPAddr(vaddr, paddr) &&
         "Paddr should always be valid to migrate a stream.");
  Addr paddrLine = makeLineAddress(paddr);
  assert(this->isPAddrHandledByMe(paddrLine) &&
         "Stream migrated to wrong LLC bank.\n");

  assert(stream->waitingDataBaseRequests == 0 &&
         "Stream migrated with waitingDataBaseRequests.");
  assert(stream->waitingIndirectElements.empty() &&
         "Stream migrated with waitingIndirectElements.");
  assert(stream->readyIndirectElements.empty() &&
         "Stream migrated with readyIndirectElements.");

  LLC_S_DPRINTF(stream->getDynamicStreamId(), "Received migrate.\n");

  // Check for if the stream is already ended.
  if (this->pendingStreamEndMsgs.count(stream->getDynamicStreamId())) {
    LLC_S_DPRINTF(stream->getDynamicStreamId(), "Ended.\n");
    delete stream;
    return;
  }

  this->streams.push_back(stream);
  this->scheduleEvent(Cycles(1));
}

void LLCStreamEngine::receiveStreamFlow(const DynamicStreamSliceId &sliceId) {
  // Simply append it to the list.
  LLCSE_DPRINTF("Received stream flow [%lu, +%lu).\n", sliceId.lhsElementIdx,
                sliceId.getNumElements());
  this->pendingStreamFlowControlMsgs.push_back(sliceId);
  this->scheduleEvent(Cycles(1));
}

void LLCStreamEngine::receiveStreamElementData(
    const DynamicStreamSliceId &sliceId, const DataBlock &dataBlock) {
  // Search through the direct streams.
  LLCDynamicStream *stream = nullptr;
  for (auto S : this->streams) {
    if (S->getDynamicStreamId() == sliceId.streamId) {
      stream = S;
      stream->waitingDataBaseRequests--;
      assert(stream->waitingDataBaseRequests >= 0 &&
             "Negative waitingDataBaseRequests.");
      break;
    }
  }
  /**
   * Since we notify the stream engine for all stream data,
   * it is possible that we don't find the stream if it is not direct stream.
   * In such case we look up the global map.
   * TODO: Really encode this in the message.
   */
  if (stream == nullptr) {
    // Try to look up the global map.
    if (LLCDynamicStream::GlobalLLCDynamicStreamMap.count(sliceId.streamId)) {
      stream = LLCDynamicStream::GlobalLLCDynamicStreamMap.at(sliceId.streamId);
    } else {
      return;
    }
  }

  LLC_SLICE_DPRINTF(sliceId, "Received element data.\n");
  // Indirect streams.
  this->processStreamDataForIndirectStreams(stream, sliceId, dataBlock);
  // Update streams.
  // ! Keep this at the end as it will modify BackingStores.
  this->processStreamDataForUpdateStream(stream, sliceId, dataBlock);
}

bool LLCStreamEngine::canMigrateStream(LLCDynamicStream *stream) const {
  /**
   * In this implementation, the LLC stream will aggressively
   * migrate to the next element bank, even the credit has only been allocated
   * to the previous element. Therefore, we do not need to check if the next
   * element is allocated.
   */
  auto nextVAddr = stream->peekVAddr();
  Addr nextPAddr;
  if (!stream->translateToPAddr(nextVAddr, nextPAddr)) {
    // If the address is faulted, we stay here.
    return false;
  }
  // Check if it is still on this bank.
  if (this->isPAddrHandledByMe(nextPAddr)) {
    // Still here.
    return false;
  }
  if (!stream->waitingIndirectElements.empty()) {
    // We are still waiting data for indirect streams.
    return false;
  }
  if (!stream->readyIndirectElements.empty()) {
    // We are still waiting for some indirect streams to be issued.
    return false;
  }
  if (stream->getStaticStream()->hasUpgradedToUpdate() &&
      stream->waitingDataBaseRequests > 0) {
    // We are still waiting to update the request.
    return false;
  }
  /**
   * ! A hack to delay migrate if there is waitingPredicatedElements for any
   * ! indirect stream.
   */
  for (auto IS : stream->indirectStreams) {
    if (!IS->waitingPredicatedElements.empty()) {
      return false;
    }
  }
  /**
   * Enforce that pointer chase stream can not migrate until the previous
   * base request comes back.
   */
  if (stream->isPointerChase() && stream->waitingDataBaseRequests > 0) {
    return false;
  }
  return true;
}

void LLCStreamEngine::wakeup() {

  // Sanity check.
  if (this->streams.size() >= 1000) {
    panic("Too many LLCStream.\n");
  }

  this->processStreamFlowControlMsg();
  this->issueStreams();
  this->migrateStreams();

  // So we limit the issue rate in issueStreams.
  while (!this->requestQueue.empty()) {
    const auto &req = this->requestQueue.front();
    this->issueStreamRequestToLLCBank(req);
    this->requestQueue.pop();
  }

  if (!this->streams.empty() || !this->migratingStreams.empty() ||
      !this->requestQueue.empty()) {
    this->scheduleEvent(Cycles(1));
  }
}

void LLCStreamEngine::processStreamFlowControlMsg() {
  auto iter = this->pendingStreamFlowControlMsgs.begin();
  auto end = this->pendingStreamFlowControlMsgs.end();
  while (iter != end) {
    const auto &msg = *iter;
    bool processed = false;
    for (auto stream : this->streams) {
      if (stream->getDynamicStreamId() == msg.streamId &&
          msg.lhsElementIdx == stream->allocatedSliceIdx) {
        // We found it.
        // Update the idx.
        LLC_S_DPRINTF(stream->getDynamicStreamId(), "Add credit %lu -> %lu.\n",
                      msg.lhsElementIdx, msg.rhsElementIdx);
        stream->addCredit(msg.getNumElements());
        processed = true;
        break;
      }
    }
    if (processed) {
      iter = this->pendingStreamFlowControlMsgs.erase(iter);
    } else {
      // LLCSE_DPRINTF("Failed to process stream credit %s [%lu, %lu).\n",
      //               msg.streamId.name.c_str(), msg.lhsElementIdx,
      //               msg.rhsElementIdx);
      ++iter;
    }
  }
}

void LLCStreamEngine::issueStreams() {

  /**
   * Enforce thresholds for issue stream requests here.
   * 1. If there are many requests in the queue, there is no need to inject more
   * packets to block the queue.
   * 2. As a sanity check, we limit the total number of infly direct requests.
   */

  if (this->streamIssueMsgBuffer->getSize(this->controller->clockEdge()) >=
      this->maxInqueueRequests) {
    return;
  }

  auto streamIter = this->streams.begin();
  auto streamEnd = this->streams.end();
  for (int i = 0, issuedStreams = 0, nStreams = this->streams.size();
       i < nStreams && issuedStreams < this->issueWidth; ++i) {
    auto curStream = streamIter;
    // Move to the next one.
    ++streamIter;
    bool issued = this->issueStream(*curStream);
    if (issued) {
      issuedStreams++;
      // Push the stream back to the end.
      this->streams.splice(streamEnd, this->streams, curStream);
    }
  }

  /**
   * Previously I only check issuedStreams for migration.
   * This assumes we only need to check migration possibility after issuing.
   * However, for pointer chase stream without indirect streams, this is not the
   * case. It maybe come migration target after receiving the previous stream
   * element data. Therefore, here I rescan all the streams to avoid deadlock.
   */

  // Scan all streams for migration target.
  streamIter = this->streams.begin();
  streamEnd = this->streams.end();
  while (streamIter != streamEnd) {
    auto stream = *streamIter;
    if (this->canMigrateStream(stream)) {
      this->migratingStreams.emplace_back(stream);
      streamIter = this->streams.erase(streamIter);
    } else {
      ++streamIter;
    }
  }
}

bool LLCStreamEngine::issueStream(LLCDynamicStream *stream) {

  /**
   * Check if we have reached issue limit for this stream.
   */
  const auto curCycle = this->controller->curCycle();
  if (curCycle < stream->prevIssuedCycle + stream->issueClearCycle) {
    // We can not issue yet.
    return false;
  }

  /**
   * Prioritize indirect elements.
   */
  if (this->issueStreamIndirect(stream)) {
    // We successfully issued an indirect element of this stream.
    // NOTE: Indirect stream issue is not counted in ClearIssueCycle.
    return true;
  }

  /**
   * After this point, try to issue base stream element.
   */

  // Enforce the per stream maxWaitingDataBaseRequests constraint.
  if (stream->waitingDataBaseRequests == stream->maxWaitingDataBaseRequests) {
    return false;
  }

  if (!stream->isNextSliceAllocated()) {
    LLC_S_DPRINTF(stream->getDynamicStreamId(),
                  "Not issue: NextSliceNotAllocated.\n");
    return false;
  }

  // Get the first element.
  Addr vaddr = stream->peekVAddr();
  Addr paddr;
  if (stream->translateToPAddr(vaddr, paddr)) {

    /**
     * ! The paddr is valid. We issue request to the LLC.
     */

    Addr paddrLine = makeLineAddress(paddr);

    /**
     * Due to the waiting indirect element, a stream may not be migrated
     * immediately after the stream engine found the next element is not
     * handled here. In such case, we simply give up and return false.
     */
    if (!this->isPAddrHandledByMe(paddr)) {
      return false;
    }

    auto sliceId = stream->consumeNextSlice();
    LLC_SLICE_DPRINTF(sliceId, "Issue.\n");
    stream->getStaticStream()->statistic.numLLCSentSlice++;

    // Register the waiting indirect elements.
    if (!stream->indirectStreams.empty()) {
      for (auto idx = sliceId.lhsElementIdx; idx < sliceId.rhsElementIdx;
           ++idx) {
        stream->waitingIndirectElements.insert(idx);
      }
    }

    // Push to the request queue.
    this->requestQueue.emplace(sliceId, paddrLine);
    stream->waitingDataBaseRequests++;

    stream->prevIssuedCycle = curCycle;
    stream->updateIssueClearCycle();
    return true;

  } else {

    /**
     * ! The paddr is not valid. We ignore this slice.
     */
    auto sliceId = stream->consumeNextSlice();
    LLC_SLICE_DPRINTF(sliceId, "Discard due to fault.\n");

    assert(stream->indirectStreams.empty() &&
           "Faulted stream with indirect streams.");
    stream->getStaticStream()->statistic.numLLCFaultSlice++;

    // This is also considered issued.
    stream->prevIssuedCycle = curCycle;
    stream->updateIssueClearCycle();
    return true;
  }
}

bool LLCStreamEngine::issueStreamIndirect(LLCDynamicStream *stream) {
  if (stream->readyIndirectElements.empty()) {
    // There is no ready indirect element to be issued.
    return false;
  }

  // Try to issue one with lowest element index.
  auto firstIndirectIter = stream->readyIndirectElements.begin();
  auto idx = firstIndirectIter->first;
  auto dynIS = firstIndirectIter->second.first;
  auto baseElementData = firstIndirectIter->second.second;

  this->generateIndirectStreamRequest(dynIS, idx, baseElementData);
  // Don't forget to release the indirect element.
  stream->readyIndirectElements.erase(firstIndirectIter);
  return true;
}

void LLCStreamEngine::generateIndirectStreamRequest(LLCDynamicStream *dynIS,
                                                    uint64_t elementIdx,
                                                    uint64_t baseElementData) {
  auto dynBS = dynIS->baseStream;
  assert(dynBS &&
         "GenerateIndirectStreamRequest can only handle indirect stream.");
  DynamicStreamSliceId sliceId;
  sliceId.streamId = dynIS->getDynamicStreamId();
  sliceId.lhsElementIdx = elementIdx;
  sliceId.rhsElementIdx = elementIdx + 1;
  auto elementSize = dynIS->getElementSize();
  LLC_SLICE_DPRINTF(sliceId, "Issue indirect slice baseElementData %llu.\n",
                    baseElementData);

  auto IS = dynIS->getStaticStream();
  const auto &indirectConfig = dynIS->configData;
  if (IS->isReduction()) {
    // This is a reduction stream.
    assert(elementIdx > 0 && "Reduction stream ElementIdx should start at 1.");

    // Perform the reduction.
    auto getBaseStreamValue = [baseElementData, dynBS,
                               dynIS](uint64_t baseStreamId) -> uint64_t {
      if (baseStreamId == dynBS->getStaticId()) {
        return baseElementData;
      }
      if (baseStreamId == dynIS->getStaticId()) {
        return dynIS->reductionValue;
      }
      assert(false && "Invalid baseStreamId.");
      return 0;
    };
    auto newReductionValue = indirectConfig.addrGenCallback->genAddr(
        elementIdx, indirectConfig.addrGenFormalParams, getBaseStreamValue);
    LLC_SLICE_DPRINTF(sliceId, "Do reduction %#x, %#x -> %#x.\n",
                      dynIS->reductionValue, baseElementData,
                      newReductionValue);
    dynIS->reductionValue = newReductionValue;

    // Do not issue any indirect request.
    return;
  }

  // Compute the address.
  auto getBaseStreamValue = [baseElementData,
                             dynBS](uint64_t baseStreamId) -> uint64_t {
    assert(baseStreamId == dynBS->getStaticId() && "Invalid baseStreamId.");
    return baseElementData;
  };
  Addr elementVAddr = indirectConfig.addrGenCallback->genAddr(
      elementIdx, indirectConfig.addrGenFormalParams, getBaseStreamValue);
  LLC_SLICE_DPRINTF(sliceId, "Generate indirect vaddr %#x, size %d.\n",
                    elementVAddr, elementSize);

  const auto blockBytes = RubySystem::getBlockSizeBytes();

  if (IS->isMerged() && IS->getStreamType() == "store") {
    // This is a merged store, we need to issue STREAM_STORE request.
    assert(elementSize <= sizeof(uint64_t) && "Oversized merged store stream.");

    int lineOffset = elementVAddr % blockBytes;
    assert(lineOffset + elementSize <= blockBytes &&
           "Multi-line merged store stream.");

    sliceId.vaddr = elementVAddr;
    sliceId.size = elementSize;
    Addr elementPAddr;
    if (dynIS->translateToPAddr(elementVAddr, elementPAddr)) {
      IS->statistic.numLLCSentSlice++;
      auto paddrLine = makeLineAddress(elementPAddr);
      // Push to the request queue.
      this->requestQueue.emplace(sliceId, paddrLine,
                                 indirectConfig.constUpdateValue);
    } else {
      panic("Faulted merged store stream.");
    }
    return;
  }

  /**
   * Finally normal indirect load stream.
   * Handle coalesced multi-line element.
   */
  auto totalSliceSize = 0;
  while (totalSliceSize < elementSize) {
    Addr curSliceVAddr = elementVAddr + totalSliceSize;
    // Make sure the slice is contained within one line.
    int lineOffset = curSliceVAddr % blockBytes;
    auto curSliceSize = std::min(elementSize - totalSliceSize,
                                 static_cast<int>(blockBytes) - lineOffset);
    // Here we set the slice vaddr and size.
    sliceId.vaddr = curSliceVAddr;
    sliceId.size = curSliceSize;
    Addr curSlicePAddr;
    if (dynIS->translateToPAddr(curSliceVAddr, curSlicePAddr)) {
      Addr curSlicePAddrLine = makeLineAddress(curSlicePAddr);
      IS->statistic.numLLCSentSlice++;

      // Push to the request queue.
      this->requestQueue.emplace(sliceId, curSlicePAddrLine);
    } else {
      // For faulted slices, we simply ignore it.
      LLC_SLICE_DPRINTF(sliceId, "Discard due to fault, vaddr %#x.\n",
                        sliceId.vaddr);
      dynIS->getStaticStream()->statistic.numLLCFaultSlice++;
    }

    totalSliceSize += curSliceSize;
  }

  return;
}

void LLCStreamEngine::issueStreamRequestToLLCBank(const LLCStreamRequest &req) {
  const auto &sliceId = req.sliceId;
  const auto paddrLine = req.paddrLine;
  auto selfMachineId = this->controller->getMachineID();
  auto destMachineId = selfMachineId;
  bool handledHere = this->isPAddrHandledByMe(req.paddrLine);
  if (handledHere) {
    LLC_SLICE_DPRINTF(sliceId,
                      "Issue [local] request vaddr %#x paddrLine %#x.\n",
                      sliceId.vaddr, paddrLine);
  } else {
    destMachineId = this->mapPaddrToLLCBank(paddrLine);
    LLC_SLICE_DPRINTF(sliceId, "Issue [remote] request to LLC%d.\n",
                      destMachineId.num);
  }

  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = paddrLine;
  msg->m_Type = req.requestType;
  msg->m_Requestor = MachineID(static_cast<MachineType>(selfMachineId.type - 1),
                               sliceId.streamId.coreId);
  msg->m_Destination.add(destMachineId);
  msg->m_MessageSize = MessageSizeType_Control;
  msg->m_sliceId = sliceId;

  Cycles latency(1); // Just use 1 cycle latency here.

  if (handledHere) {
    this->streamIssueMsgBuffer->enqueue(
        msg, this->controller->clockEdge(),
        this->controller->cyclesToTicks(latency));
  } else {
    this->streamIndirectIssueMsgBuffer->enqueue(
        msg, this->controller->clockEdge(),
        this->controller->cyclesToTicks(latency));
  }
}

void LLCStreamEngine::issueStreamAckToMLC(const DynamicStreamSliceId &sliceId) {

  auto selfMachineId = this->controller->getMachineID();
  MachineID mlcMachineId(static_cast<MachineType>(selfMachineId.type - 1),
                         sliceId.streamId.coreId);

  auto msg = std::make_shared<ResponseMsg>(this->controller->clockEdge());
  msg->m_Type = CoherenceResponseType_STREAM_ACK;
  msg->m_Sender = selfMachineId;
  msg->m_Destination.add(mlcMachineId);
  msg->m_MessageSize = MessageSizeType_Response_Control;
  msg->m_sliceId = sliceId;

  /**
   * This should match with LLC controller l2_response_latency.
   * TODO: Really get this value from the controller.
   */
  Cycles latency(2);
  this->streamResponseMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(latency));
}

void LLCStreamEngine::migrateStreams() {
  auto streamIter = this->migratingStreams.begin();
  auto streamEnd = this->migratingStreams.end();
  int migrated = 0;
  while (streamIter != streamEnd && migrated < this->migrateWidth) {
    auto stream = *streamIter;
    assert(this->canMigrateStream(stream) && "Can't migrate stream.");
    // We do not migrate the stream if it has unprocessed indirect elements.
    this->migrateStream(stream);
    streamIter = this->migratingStreams.erase(streamIter);
    migrated++;
  }
}

void LLCStreamEngine::migrateStream(LLCDynamicStream *stream) {

  // Remember to clear the waitingDataBaseRequests becase we may aggressively
  // migrate direct streams (not pointer chase).
  stream->waitingDataBaseRequests = 0;

  // Create the migrate request.
  Addr vaddr = stream->peekVAddr();
  Addr paddr;
  assert(stream->translateToPAddr(vaddr, paddr) &&
         "Migrating streams should have valid paddr.");
  Addr paddrLine = makeLineAddress(paddr);
  auto selfMachineId = this->controller->getMachineID();
  auto addrMachineId =
      this->controller->mapAddressToLLC(paddrLine, selfMachineId.type);

  LLC_S_DPRINTF(stream->getDynamicStreamId(), "Migrate to LLC%d.\n",
                addrMachineId.num);

  auto msg =
      std::make_shared<StreamMigrateRequestMsg>(this->controller->clockEdge());
  msg->m_addr = paddrLine;
  msg->m_Type = CoherenceRequestType_GETS;
  msg->m_Requestor = selfMachineId;
  msg->m_Destination.add(addrMachineId);
  msg->m_MessageSize = MessageSizeType_Data;
  msg->m_Stream = stream;

  Cycles latency(1); // Just use 1 cycle latency here.

  this->streamMigrateMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(latency));
}

MachineID LLCStreamEngine::mapPaddrToLLCBank(Addr paddr) const {
  auto selfMachineId = this->controller->getMachineID();
  auto addrMachineId =
      this->controller->mapAddressToLLC(paddr, selfMachineId.type);
  return addrMachineId;
}

bool LLCStreamEngine::isPAddrHandledByMe(Addr paddr) const {
  auto selfMachineId = this->controller->getMachineID();
  auto addrMachineId =
      this->controller->mapAddressToLLC(paddr, selfMachineId.type);
  return addrMachineId == selfMachineId;
}

void LLCStreamEngine::print(std::ostream &out) const {}

void LLCStreamEngine::receiveStreamIndirectRequest(const RequestMsg &req) {
  // Simply copy and inject the msg to L1 request in.
  const auto &sliceId = req.m_sliceId;
  assert(sliceId.isValid() && "Invalid stream slice for indirect request.");

  LLC_SLICE_DPRINTF(sliceId, "Inject [indirect] request.\n");

  auto msg = std::make_shared<RequestMsg>(req);
  Cycles latency(1); // Just use 1 cycle latency here.

  this->streamIssueMsgBuffer->enqueue(msg, this->controller->clockEdge(),
                                      this->controller->cyclesToTicks(latency));
}

void LLCStreamEngine::processStreamDataForIndirectStreams(
    LLCDynamicStreamPtr stream, const DynamicStreamSliceId &sliceId,
    const DataBlock &dataBlock) {
  if (stream->indirectStreams.empty() &&
      stream->waitingPredicatedElements.empty()) {
    return;
  }
  for (auto idx = sliceId.lhsElementIdx; idx < sliceId.rhsElementIdx; ++idx) {
    auto elementData =
        this->extractElementDataFromSlice(stream, sliceId, idx, dataBlock);
    LLC_S_DPRINTF(sliceId.streamId, "Received element %lu data %lu.\n", idx,
                  elementData);

    // Add them to the ready indirect list.
    if (stream->waitingIndirectElements.count(idx) != 0) {
      for (auto IS : stream->indirectStreams) {

        /**
         * If the indirect stream is behind one iteration, base element of
         * iteration i should trigger the indirect element of iteration i + 1.
         * Also we should be careful to not overflow the boundary.
         */
        auto indirectElementIdx = idx;
        if (IS->isOneIterationBehind()) {
          indirectElementIdx = idx + 1;
        }
        auto indirectTripCount = IS->configData.totalTripCount;
        if (indirectTripCount != -1 && indirectElementIdx > indirectTripCount) {
          // Ignore overflow elements.
          continue;
        }

        /**
         * Check if the stream has predication.
         */
        if (IS->isPredicated()) {
          assert(!IS->isOneIterationBehind() && "How to handle this?");
          // Push the element to the predicate list.
          // We add the element to the predicateElements.
          IS->predicateStream->waitingPredicatedElements
              .emplace(std::piecewise_construct, std::forward_as_tuple(idx),
                       std::forward_as_tuple())
              .first->second.emplace_back(IS, elementData);
        } else {
          // Not predicated, add to readyElements.
          assert(stream->baseStream == nullptr);
          stream->readyIndirectElements.emplace(
              std::piecewise_construct,
              std::forward_as_tuple(indirectElementIdx),
              std::forward_as_tuple(IS, elementData));
        }
      }
      // Don't forget to erase it from the waiting list.
      stream->waitingIndirectElements.erase(idx);
    }
    // Now we handle any predication.
    if (stream->configData.predCallback) {
      GetStreamValueFunc getStreamValue =
          [elementData, stream](uint64_t streamId) -> uint64_t {
        assert(streamId == stream->getStaticId() &&
               "Mismatch stream id for predication.");
        return elementData;
      };
      auto params = convertFormalParamToParam(
          stream->configData.predFormalParams, getStreamValue);
      bool predicatedTrue =
          stream->configData.predCallback->invoke(params) & 0x1;
      auto predicatedIter = stream->waitingPredicatedElements.find(idx);
      if (predicatedIter != stream->waitingPredicatedElements.end()) {
        for (auto &predEntry : predicatedIter->second) {
          auto dynPredS = predEntry.first;
          auto predS = dynPredS->getStaticStream();
          auto predBaseData = predEntry.second;
          LLC_S_DPRINTF(sliceId.streamId, "Predicate %d %d: %s.\n",
                        predicatedTrue, dynPredS->isPredicatedTrue(),
                        dynPredS->getDynamicStreamId());
          if (dynPredS->isPredicatedTrue() == predicatedTrue) {
            predS->statistic.numLLCPredYSlice++;
            // Predicated match, add to ready list.
            if (stream->baseStream) {
              /**
               * The predication is from an indirect stream, this is for
               * pattern: if (a[b[i]]) c[xx]; Since this is an indirect
               * stream, it is likely that we are in a remote LLC bank where
               * a[b[i]] is sitting. We would like to directly generate the
               * address and inject to the requestQueue here.
               */
              this->generateIndirectStreamRequest(dynPredS, idx, predBaseData);
            } else {
              /**
               * The predication is from a direct stream, this is for pattern:
               * if (a[i]) b[i];
               * There is no data dependence between these two streams.
               * In such case we add to readyIndirectElements and waiting to
               * be issued.
               */
              stream->readyIndirectElements.emplace(
                  std::piecewise_construct, std::forward_as_tuple(idx),
                  std::forward_as_tuple(dynPredS, predBaseData));
            }
          } else {
            // This element is predicated off.
            predS->statistic.numLLCPredNSlice++;
            // if (predS->isMerged() && predS->getStreamType() == "store") {
            //   // This is a predicated off merged store, we have to send
            //   // STREAM_ACK.
            //   DynamicStreamSliceId sliceId;
            //   sliceId.streamId = dynPredS->getDynamicStreamId();
            //   sliceId.lhsElementIdx = idx;
            //   sliceId.rhsElementIdx = idx + 1;
            //   this->issueStreamAckToMLC(sliceId);
            // }
          }
        }
        stream->waitingPredicatedElements.erase(predicatedIter);
      }

    } else {
      assert(stream->waitingPredicatedElements.empty() &&
             "No predCallback for predicated elements.");
    }
  }
}

void LLCStreamEngine::processStreamDataForUpdateStream(
    LLCDynamicStreamPtr stream, const DynamicStreamSliceId &sliceId,
    const DataBlock &dataBlock) {

  if (!stream->getStaticStream()->hasUpgradedToUpdate()) {
    return;
  }

  uint64_t updateValue = stream->configData.constUpdateValue;
  for (auto idx = sliceId.lhsElementIdx; idx < sliceId.rhsElementIdx; ++idx) {
    this->updateElementData(stream, idx, updateValue);
    LLC_S_DPRINTF(sliceId.streamId, "Update element %lu value %lu.\n", idx,
                  updateValue);
  }
}

uint64_t LLCStreamEngine::extractElementDataFromSlice(
    LLCDynamicStreamPtr stream, const DynamicStreamSliceId &sliceId,
    uint64_t elementIdx, const DataBlock &dataBlock) {
  // TODO: Handle multi-line element.
  Addr elementVAddr;
  if (stream->baseStream) {
    // This is indirect stream, directly use sliceId.vaddr as elementVAddr.
    elementVAddr = sliceId.vaddr;
  } else {
    elementVAddr = stream->slicedStream.getElementVAddr(elementIdx);
  }
  auto elementSize = stream->getElementSize();
  auto elementLineOffset = elementVAddr % RubySystem::getBlockSizeBytes();
  assert(elementLineOffset + elementSize <= RubySystem::getBlockSizeBytes() &&
         "Cannot support multi-line element with indirect streams yet.");
  assert(elementSize <= sizeof(uint64_t) && "At most 8 byte element size.");

  uint64_t elementData = 0;
  auto rubySystem = this->controller->params()->ruby_system;
  if (rubySystem->getAccessBackingStore()) {
    // Get the data from backing store.
    Addr elementPAddr;
    assert(stream->translateToPAddr(elementVAddr, elementPAddr) &&
           "Failed to translate address for accessing backing storage.");
    RequestPtr req(new Request(elementPAddr, elementSize, 0, 0 /* MasterId */,
                               0 /* InstSeqNum */, 0 /* contextId */));
    PacketPtr pkt = Packet::createRead(req);
    uint8_t *pktData = new uint8_t[req->getSize()];
    pkt->dataDynamic(pktData);
    rubySystem->getPhysMem()->functionalAccess(pkt);
    for (auto byteOffset = 0; byteOffset < elementSize; ++byteOffset) {
      *(reinterpret_cast<uint8_t *>(&elementData) + byteOffset) =
          pktData[byteOffset];
    }
    delete pkt;
  } else {
    // Get the data from the cache line.
    // Copy the data in.
    // TODO: How do we handle sign for data type less than 8 bytes?
    for (auto byteOffset = 0; byteOffset < elementSize; ++byteOffset) {
      *(reinterpret_cast<uint8_t *>(&elementData) + byteOffset) =
          dataBlock.getByte(byteOffset + elementLineOffset);
    }
  }
  return elementData;
}

void LLCStreamEngine::updateElementData(LLCDynamicStreamPtr stream,
                                        uint64_t elementIdx,
                                        uint64_t updateValue) {
  // TODO: Handle multi-line element.
  auto elementVAddr = stream->slicedStream.getElementVAddr(elementIdx);
  auto elementSize = stream->getElementSize();
  auto elementLineOffset = elementVAddr % RubySystem::getBlockSizeBytes();
  assert(elementLineOffset + elementSize <= RubySystem::getBlockSizeBytes() &&
         "Cannot support multi-line element with indirect streams yet.");
  assert(elementSize <= sizeof(uint64_t) && "At most 8 byte element size.");

  auto rubySystem = this->controller->params()->ruby_system;
  if (rubySystem->getAccessBackingStore()) {

    /**
     * ! The ruby system uses the BackingStore. However, we can not
     * update it here, as then the RubySequencer will read the updated
     * value for the StreamEngine.
     */
    return;

    // // Get the data from backing store.
    // Addr elementPAddr;
    // assert(stream->translateToPAddr(elementVAddr, elementPAddr) &&
    //        "Failed to translate address for accessing backing storage.");
    // RequestPtr req(new Request(elementPAddr, elementSize, 0, 0 /* MasterId
    // */,
    //                            0 /* InstSeqNum */, 0 /* contextId */));
    // PacketPtr pkt = Packet::createWrite(req);
    // uint8_t *pktData = new uint8_t[req->getSize()];
    // memcpy(pktData, reinterpret_cast<uint8_t *>(&updateValue),
    // elementSize); pkt->dataDynamic(pktData);
    // rubySystem->getPhysMem()->functionalAccess(pkt);
    // delete pkt;
  } else {
    panic("Do not support UpdateStream without BackingStore.");
  }
}