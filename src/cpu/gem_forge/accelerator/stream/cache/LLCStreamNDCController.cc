#include "LLCStreamNDCController.hh"

#include "mem/simple_mem.hh"

#include "debug/StreamNearDataComputing.hh"

#define DEBUG_TYPE StreamNearDataComputing
#include "../stream_log.hh"

#define LLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(StreamNearDataComputing, "[LLC_SE%d]: " format,                      \
          llcSE->controller->getMachineID().num, ##args)

#define LLC_NDC_DPRINTF(ndc, format, args...)                                  \
  LLCSE_DPRINTF("%s: " format, (ndc)->entryIdx, ##args)

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
  auto dynS = S->getDynamicStream(streamNDC->entryIdx.streamId);
  if (!dynS) {
    LLC_NDC_DPRINTF(streamNDC, "Discard NDC req as DynS released.\n");
    return;
  }

  auto element = dynS->getElementByIdx(streamNDC->entryIdx.entryIdx);
  if (!element) {
    LLC_NDC_DPRINTF(streamNDC, "Discard NDC req as Element released.\n");
    return;
  }

  LLC_NDC_DPRINTF(streamNDC, "Receive NDC req.\n");
  auto elementIdx = streamNDC->entryIdx.entryIdx;
  DynamicStreamSliceId sliceId;
  sliceId.getDynStreamId() = streamNDC->entryIdx.streamId;
  sliceId.getStartIdx() = elementIdx;
  sliceId.getEndIdx() = elementIdx + 1;
  sliceId.vaddr = streamNDC->vaddr;
  sliceId.size = S->getMemElementSize();
  auto vaddrLine = makeLineAddress(streamNDC->vaddr);
  auto paddrLine = makeLineAddress(streamNDC->paddr);
  llcSE->enqueueRequest(S->getCPUDelegator(), sliceId, vaddrLine, paddrLine,
                        CoherenceRequestType_STREAM_STORE);

  auto &contexts =
      this->inflyNDCContextMap
          .emplace(std::piecewise_construct,
                   std::forward_as_tuple(streamNDC->entryIdx.streamId),
                   std::forward_as_tuple())
          .first->second;
  contexts.emplace_back(streamNDC);
}

LLCStreamNDCController::NDCContext &
LLCStreamNDCController::getContextFromSliceId(
    const DynamicStreamSliceId &sliceId) {
  auto iter = this->inflyNDCContextMap.find(sliceId.getDynStreamId());
  if (iter == this->inflyNDCContextMap.end()) {
    panic("Failed to find context list for %s.", sliceId);
  }
  for (auto &context : iter->second) {
    if (context.ndc->entryIdx.entryIdx == sliceId.getStartIdx()) {
      return context;
    }
  }
  panic("Failed to find context for %s.", sliceId);
}

void LLCStreamNDCController::eraseContextFromSliceId(
    const DynamicStreamSliceId &sliceId) {
  auto iter = this->inflyNDCContextMap.find(sliceId.getDynStreamId());
  if (iter == this->inflyNDCContextMap.end()) {
    panic("Failed to find context list for %s.", sliceId);
  }
  auto &contexts = iter->second;
  for (auto contextIter = contexts.begin(); contextIter != contexts.end();
       ++contextIter) {
    if (contextIter->ndc->entryIdx.entryIdx == sliceId.getStartIdx()) {
      contexts.erase(contextIter);
      if (contexts.empty()) {
        this->inflyNDCContextMap.erase(iter);
      }
      return;
    }
  }
  panic("Failed to find context for %s.", sliceId);
}

void LLCStreamNDCController::receiveStreamData(
    const DynamicStreamSliceId &sliceId, const DataBlock &dataBlock,
    const DataBlock &storeValueBlock) {

  auto &context = this->getContextFromSliceId(sliceId);
  LLC_NDC_DPRINTF(context.ndc, "Receive stream data.\n");

  auto S = context.ndc->stream;
  auto dynS = S->getDynamicStream(context.ndc->entryIdx.streamId);
  assert(dynS && "Missing DynS for NDC.");

  auto atomicOp =
      S->setupAtomicOp(context.ndc->entryIdx, S->getMemElementSize(),
                       dynS->storeFormalParams, getStreamValueFail);

  /**
   * Create the packet.
   */
  auto pkt =
      llcSE->createAtomicPacket(context.ndc->vaddr, context.ndc->paddr,
                                S->getMemElementSize(), std::move(atomicOp));

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

void LLCStreamNDCController::issueStreamNDCResponseToMLC(
    const DynamicStreamSliceId &sliceId, Addr paddrLine, const uint8_t *data,
    int dataSize, int payloadSize, int lineOffset, bool forceIdea) {

  auto msg = llcSE->createStreamMsgToMLC(
      sliceId, CoherenceResponseType_STREAM_NDC, paddrLine, data, dataSize,
      payloadSize, lineOffset);
  llcSE->issueStreamMsgToMLC(msg, forceIdea);
}