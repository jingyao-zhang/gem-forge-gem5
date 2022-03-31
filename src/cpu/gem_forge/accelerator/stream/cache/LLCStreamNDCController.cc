#include "LLCStreamNDCController.hh"
#include "StreamRequestBuffer.hh"

#include "mem/simple_mem.hh"

#include "debug/StreamNearDataComputing.hh"

#define DEBUG_TYPE StreamNearDataComputing
#include "../stream_log.hh"

#define LLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(StreamNearDataComputing, "[LLC_SE%d]: " format,                      \
          llcSE->controller->getMachineID().num, ##args)
#define LLCSE_PANIC(format, args...)                                           \
  panic("[LLC_SE%d]: " format, llcSE->controller->getMachineID().num, ##args)

#define LLC_NDC_DPRINTF(ndc, format, args...)                                  \
  LLCSE_DPRINTF("%s: " format, (ndc)->entryIdx, ##args)
#define LLC_NDC_PANIC(ndc, format, args...)                                    \
  LLCSE_PANIC("%s: " format, (ndc)->entryIdx, ##args)

LLCStreamNDCController::NDCContextMapT
    LLCStreamNDCController::inflyNDCContextMap;

LLCStreamNDCController::LLCStreamNDCController(LLCStreamEngine *_llcSE)
    : llcSE(_llcSE) {}

void LLCStreamNDCController::receiveStreamNDCRequest(PacketPtr pkt) {
  llcSE->initializeTranslationBuffer();
  this->processStreamNDCRequest(pkt);
  // Release memory.
  *(pkt->getPtr<StreamNDCPacketPtr>()) = nullptr;
  delete pkt;
}

void LLCStreamNDCController::processStreamNDCRequest(PacketPtr pkt) {

  // Try get the dynamic stream.
  auto streamNDC = *(pkt->getPtr<StreamNDCPacketPtr>());
  auto S = streamNDC->stream;
  auto dynS = S->getDynStream(streamNDC->entryIdx.streamId);
  if (!dynS) {
    LLC_NDC_DPRINTF(streamNDC, "Discard NDC req as DynS released.\n");
    return;
  }

  auto element = dynS->getElemByIdx(streamNDC->entryIdx.entryIdx);
  if (!element) {
    LLC_NDC_DPRINTF(streamNDC, "Discard NDC req as Element released.\n");
    return;
  }

  LLC_NDC_DPRINTF(streamNDC, "Receive NDC req.\n");
  auto elementIdx = streamNDC->entryIdx.entryIdx;
  DynStreamSliceId sliceId;
  sliceId.getDynStrandId() = DynStrandId(streamNDC->entryIdx.streamId);
  sliceId.getStartIdx() = elementIdx;
  sliceId.getEndIdx() = elementIdx + 1;
  sliceId.vaddr = streamNDC->vaddr;
  sliceId.size = S->getMemElementSize();
  auto vaddrLine = makeLineAddress(streamNDC->vaddr);
  auto paddrLine = makeLineAddress(streamNDC->paddr);
  auto requestType = CoherenceRequestType_STREAM_STORE;
  if (streamNDC->isForward) {
    requestType = CoherenceRequestType_GETH;
  }
  llcSE->enqueueRequest(S, sliceId, vaddrLine, paddrLine,
                        llcSE->myMachineType(), requestType);
}

void LLCStreamNDCController::allocateContext(
    AbstractStreamAwareController *mlcController,
    StreamNDCPacketPtr &streamNDC) {
  auto &contexts =
      inflyNDCContextMap
          .emplace(std::piecewise_construct,
                   std::forward_as_tuple(streamNDC->entryIdx.streamId),
                   std::forward_as_tuple())
          .first->second;

  auto S = streamNDC->stream;
  auto size = S->getMemElementSize();
  auto element = std::make_shared<LLCStreamElement>(
      S, mlcController, DynStrandId(streamNDC->entryIdx.streamId),
      streamNDC->entryIdx.entryIdx, streamNDC->vaddr, size,
      true /* isNDCElement */);

  // Add all the base elements.
  for (const auto &forward : streamNDC->expectedForwardPackets) {
    auto forwardElement = std::make_shared<LLCStreamElement>(
        forward->stream, mlcController, DynStrandId(forward->entryIdx.streamId),
        forward->entryIdx.entryIdx, forward->vaddr,
        forward->stream->getMemElementSize(), true /* isNDCElement */
    );
    element->baseElements.push_back(forwardElement);
  }

  contexts.emplace_back(streamNDC, element);
}

LLCStreamNDCController::NDCContext *
LLCStreamNDCController::getContextFromSliceId(const DynStreamSliceId &sliceId) {
  auto iter = inflyNDCContextMap.find(sliceId.getDynStreamId());
  if (iter == inflyNDCContextMap.end()) {
    return nullptr;
  }
  for (auto &context : iter->second) {
    if (context.ndc->entryIdx.entryIdx == sliceId.getStartIdx()) {
      return &context;
    }
  }
  return nullptr;
}

void LLCStreamNDCController::eraseContextFromSliceId(
    const DynStreamSliceId &sliceId) {
  auto iter = inflyNDCContextMap.find(sliceId.getDynStreamId());
  if (iter == inflyNDCContextMap.end()) {
    panic("Failed to find context list for %s.", sliceId);
  }
  auto &contexts = iter->second;
  for (auto contextIter = contexts.begin(); contextIter != contexts.end();
       ++contextIter) {
    if (contextIter->ndc->entryIdx.entryIdx == sliceId.getStartIdx()) {
      contexts.erase(contextIter);
      if (contexts.empty()) {
        inflyNDCContextMap.erase(iter);
      }
      return;
    }
  }
  panic("Failed to find context for %s.", sliceId);
}

void LLCStreamNDCController::receiveStreamData(
    const DynStreamSliceId &sliceId, const DataBlock &dataBlock,
    const DataBlock &storeValueBlock) {

  auto *context = this->getContextFromSliceId(sliceId);
  if (!context) {
    // This is an NDC context.
    return;
  }
  LLC_NDC_DPRINTF(context->ndc, "Receive stream data.\n");

  context->cacheLineReady = true;
  this->handleNDC(*context, sliceId, dataBlock);
}

void LLCStreamNDCController::handleNDC(NDCContext &context,
                                       const DynStreamSliceId &sliceId,
                                       const DataBlock &dataBlock) {
  LLC_NDC_DPRINTF(context.ndc,
                  "[NDC] CacheLineReady %d. Got %d of %d Forwards.\n",
                  context.cacheLineReady, context.receivedForward,
                  context.ndc->expectedForwardPackets.size());
  if (context.receivedForward < context.ndc->expectedForwardPackets.size() ||
      !context.cacheLineReady) {
    return;
  }
  auto S = context.ndc->stream;
  if (S->isAtomicComputeStream()) {
    this->handleAtomicNDC(context, sliceId);
  } else if (S->isLoadStream() && context.ndc->isForward) {
    this->handleForwardNDC(context, sliceId, dataBlock);
  } else if (S->isStoreComputeStream()) {
    this->handleStoreNDC(context);
  } else {
    LLC_NDC_PANIC(context.ndc, "Illegal NDC Stream Type.");
  }
}

void LLCStreamNDCController::handleAtomicNDC(NDCContext &context,
                                             const DynStreamSliceId &sliceId) {

  auto S = context.ndc->stream;
  auto dynS = S->getDynStream(context.ndc->entryIdx.streamId);
  if (!dynS) {
    LLC_NDC_PANIC(context.ndc, "Missing DynS for NDC response.");
  }

  /**
   * Create the packet.
   */
  auto pkt = llcSE->createAtomicPacket(context.ndc->vaddr, context.ndc->paddr,
                                       S->getMemElementSize(),
                                       std::move(context.ndc->atomicOp));

  /**
   * Send to backing store to perform atomic op.
   */
  auto rubySystem = llcSE->controller->params()->ruby_system;
  assert(rubySystem->getAccessBackingStore() &&
         "Do not support atomicrmw stream without BackingStore.");

  rubySystem->getPhysMem()->functionalAccess(pkt);
  LLC_NDC_DPRINTF(
      context.ndc,
      "Atomic response, isWrite %d, vaddr %#x, paddr %#x, size %d.\n",
      pkt->isWrite(), pkt->req->getVaddr(), pkt->getAddr(), pkt->getSize());

  uint64_t loadedValue = 0;
  // bool memoryModified = false;
  {
    auto atomicOp = dynamic_cast<StreamAtomicOp *>(pkt->getAtomicOp());
    loadedValue = atomicOp->getLoadedValue().front();
    // memoryModified = atomicOp->modifiedMemory();
  }

  // Send back the response.
  auto paddrLine = makeLineAddress(context.ndc->paddr);
  auto lineOffset = context.ndc->paddr - paddrLine;
  auto dataSize = S->getCoreElementSize();
  auto payloadSize = S->getCoreElementSize();
  this->issueStreamNDCResponseToMLC(
      sliceId, paddrLine, reinterpret_cast<uint8_t *>(&loadedValue), dataSize,
      payloadSize, lineOffset, false /* forceIdea */);

  delete pkt;
  this->eraseContextFromSliceId(sliceId);
}

void LLCStreamNDCController::handleStoreNDC(NDCContext &context) {

  /**
   * So far let's just push the element into LCStreamEngine.
   */
  llcSE->pushReadyComputation(context.element);
}

void LLCStreamNDCController::handleForwardNDC(NDCContext &context,
                                              const DynStreamSliceId &sliceId,
                                              const DataBlock &dataBlock) {

  auto S = context.ndc->stream;
  auto dynS = S->getDynStream(context.ndc->entryIdx.streamId);
  if (!dynS) {
    LLC_NDC_PANIC(context.ndc, "Missing DynS for NDC response.");
  }

  // Forward the cache line to the receiver bank.
  auto paddrLine = makeLineAddress(context.ndc->receiverPAddr);
  auto selfMachineId = llcSE->controller->getMachineID();
  auto destMachineId = selfMachineId;
  bool handledHere =
      llcSE->isPAddrHandledByMe(paddrLine, selfMachineId.getType());
  auto requestType = CoherenceRequestType_STREAM_FORWARD;
  if (handledHere) {
    LLC_NDC_DPRINTF(context.ndc,
                    "NDC Forward [local] %#x paddrLine %#x value %s.\n",
                    context.ndc->receiverVAddr, paddrLine, dataBlock);
  } else {
    destMachineId = llcSE->mapPaddrToSameLevelBank(paddrLine);
    LLC_NDC_DPRINTF(context.ndc, "NDC Fwd [remote] to %s value %s.\n",
                    MachineIDToString(destMachineId), dataBlock);
  }

  auto msg = std::make_shared<RequestMsg>(llcSE->controller->clockEdge());
  msg->m_addr = paddrLine;
  msg->m_Type = requestType;
  msg->m_Requestors.add(
      MachineID(static_cast<MachineType>(selfMachineId.type - 1),
                sliceId.getDynStreamId().coreId));
  msg->m_Destination.add(destMachineId);
  msg->m_MessageSize = MessageSizeType_Control;
  msg->m_sliceIds.add(sliceId);
  msg->m_DataBlk = dataBlock;
  msg->m_sendToStrandId = DynStrandId(context.ndc->receiverEntryIdx.streamId);
  /**
   * We model special size for StreamForward request.
   */
  msg->m_MessageSize =
      llcSE->controller->getMessageSizeType(S->getMemElementSize());

  // Quick path for StreamForward if it is handled here.
  if (handledHere) {
    llcSE->receiveStreamForwardRequest(*msg);
  } else {
    llcSE->indReqBuffer->pushRequest(msg);
  }

  this->eraseContextFromSliceId(sliceId);
}

void LLCStreamNDCController::receiveStreamForwardRequest(
    const RequestMsg &msg) {
  const auto &sliceId = msg.m_sliceIds.singleSliceId();
  const auto &recvDynId = msg.m_sendToStrandId;

  LLCSE_DPRINTF("Received NDC Forward %s -> %s.\n", sliceId, recvDynId);

  DynStreamSliceId recvSliceId;
  recvSliceId.getDynStrandId() = recvDynId;
  recvSliceId.getStartIdx() = sliceId.getStartIdx();
  recvSliceId.getEndIdx() = sliceId.getEndIdx();

  auto *context = this->getContextFromSliceId(recvSliceId);
  if (!context) {
    // This is not an NDC forward.
    return;
  }

  auto S = context->ndc->stream;
  bool setBaseElementValue = false;
  for (auto &baseElement : context->element->baseElements) {
    if (baseElement->strandId == sliceId.getDynStrandId()) {
      S->getCPUDelegator()->readFromMem(baseElement->vaddr, baseElement->size,
                                        baseElement->getUInt8Ptr());
      baseElement->addReadyBytes(baseElement->size);
      setBaseElementValue = true;
      break;
    }
  }
  if (!setBaseElementValue) {
    LLC_NDC_PANIC(context->ndc,
                  "Failed to find BaseElement for NDCForward from %s.",
                  sliceId);
  }

  context->receivedForward++;
  this->handleNDC(*context, recvSliceId, msg.m_DataBlk);
}

void LLCStreamNDCController::issueStreamNDCResponseToMLC(
    const DynStreamSliceId &sliceId, Addr paddrLine, const uint8_t *data,
    int dataSize, int payloadSize, int lineOffset, bool forceIdea) {

  auto msg = llcSE->createStreamMsgToMLC(
      sliceId, CoherenceResponseType_STREAM_NDC, paddrLine, data, dataSize,
      payloadSize, lineOffset);
  llcSE->issueStreamMsgToMLC(msg, forceIdea);
}

bool LLCStreamNDCController::computeStreamElementValue(
    const LLCStreamElementPtr &element, StreamValue &result) {

  auto S = element->S;
  auto dynS = S->getDynStream(element->strandId.dynStreamId);
  if (!dynS) {
    LLC_ELEMENT_DPRINTF_(StreamNearDataComputing, element,
                         "Discard LLC NDC Element as no DynS.");
    return false;
  }

  auto getBaseStreamValue = [&element](uint64_t baseStreamId) -> StreamValue {
    return element->getBaseStreamValue(baseStreamId);
  };

  if (S->isStoreComputeStream()) {
    Cycles latency = dynS->storeCallback->getEstimatedLatency();
    auto params =
        convertFormalParamToParam(dynS->storeFormalParams, getBaseStreamValue);
    auto storeValue = dynS->storeCallback->invoke(params);

    LLC_ELEMENT_DPRINTF_(StreamNearDataComputing, element,
                         "[Latency %llu] Compute StoreValue %s.\n", latency,
                         storeValue);
    result = storeValue;
    return true;

  } else {
    LLC_ELEMENT_PANIC(element, "No Computation for this stream.");
  }
}

void LLCStreamNDCController::completeComputation(
    const LLCStreamElementPtr &element, const StreamValue &value) {
  auto S = element->S;
  element->doneComputation();
  /**
   * LoadComputeStream store computed value in LoadComputeValue.
   * IndirectReductionStream separates compuation from charging the latency.
   */
  element->setValue(value);

  DynStreamSliceId sliceId;
  sliceId.getDynStrandId() = element->strandId;
  sliceId.getStartIdx() = element->idx;
  sliceId.getEndIdx() = element->idx + 1;

  auto *context = this->getContextFromSliceId(sliceId);
  if (!context) {
    LLC_ELEMENT_PANIC(element, "Missing NDC Context.");
  }

  if (S->isStoreComputeStream()) {
    /**
     * Perform the functional write and send back response.
     */
    S->getCPUDelegator()->writeToMem(context->ndc->vaddr,
                                     S->getMemElementSize(), value.uint8Ptr());

    /**
     * Send back the response, which is just an Ack.
     */
    auto paddrLine = makeLineAddress(context->ndc->paddr);
    auto lineOffset = 0;
    auto dataSize = 4;
    auto payloadSize = 0;
    this->issueStreamNDCResponseToMLC(sliceId, paddrLine, nullptr, dataSize,
                                      payloadSize, lineOffset,
                                      false /* forceIdea */);

  } else {
    LLC_NDC_PANIC(context->ndc,
                  "Illegal NDC StreamType in CompleteComputation.");
  }
  this->eraseContextFromSliceId(sliceId);
}