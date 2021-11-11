#include "StreamRequestBuffer.hh"

#include "debug/LLCRubyStreamBase.hh"
#include "debug/LLCRubyStreamMulticast.hh"
#define DEBUG_TYPE LLCRubyStreamBase
#include "../stream_log.hh"

StreamRequestBuffer::StreamRequestBuffer(
    AbstractStreamAwareController *_controller, MessageBuffer *_outBuffer,
    Cycles _latency, int _maxInqueueRequestsPerStream,
    int _maxMulticastReqPerMsg, int _multicastBankGroupSize)
    : controller(_controller), outBuffer(_outBuffer), latency(_latency),
      maxInqueueRequestsPerStream(_maxInqueueRequestsPerStream),
      maxMulticastReqPerMsg(_maxMulticastReqPerMsg),
      multicastBankGroupSize(_multicastBankGroupSize) {
  this->outBuffer->registerDequeueCallbackOnMsg(
      [this](MsgPtr msg) -> void { this->dequeue(msg); });
}

void StreamRequestBuffer::pushRequest(RequestPtr request) {
  /**
   * See if we should and can multicast this request.
   * If not, we try to add to multicast group.
   */
  if (this->shouldTryMulticast(request)) {
    if (this->tryMulticast(request)) {
      return;
    }
    auto groupId = this->getMulticastGroupId(request);
    auto groupIter = this->getOrInitMulticastGroupInqueueReq(groupId);
    groupIter->second.insert(request);
  }
  const auto &sliceId = request->m_sliceIds.singleSliceId();
  auto inqueueIter = this->getOrInitInqueueState(request);
  auto &inqueueState = inqueueIter->second;
  if (inqueueState.inqueueRequests == this->maxInqueueRequestsPerStream) {
    // Reached maximum inqueue requests. Buffered.
    inqueueState.buffered.push_back(request);
    this->totalBufferedRequests++;
    LLC_SLICE_DPRINTF(
        sliceId, "[ReqBuffer] Delayed as Inqueued %d Buffered %lu.\n",
        inqueueState.inqueueRequests, inqueueState.buffered.size());
  } else {
    // Can directly enqueue.
    this->enqueue(request, inqueueState.inqueueRequests);
  }
}

void StreamRequestBuffer::dequeue(MsgPtr msg) {
  auto request = std::dynamic_pointer_cast<RequestMsg>(msg);
  if (!request) {
    LLC_SE_PANIC("Message should be RequestMsg.");
  }

  if (this->shouldTryMulticast(request)) {
    auto groupId = this->getMulticastGroupId(request);
    auto groupIter = this->getOrInitMulticastGroupInqueueReq(groupId);
    groupIter->second.erase(request);
  }

  const auto &sliceId = request->m_sliceIds.singleSliceId();
  auto inqueueIter = this->getOrInitInqueueState(request);
  auto &inqueueState = inqueueIter->second;
  if (inqueueState.inqueueRequests == 0) {
    LLC_SLICE_PANIC(sliceId, "[ReqBuffer] Dequeued with Zero InqueueRequests.");
  }
  inqueueState.inqueueRequests--;
  LLC_SLICE_DPRINTF(sliceId, "[ReqBuffer] Dequeued. Inqueue %d Buffered %lu.\n",
                    inqueueState.inqueueRequests, inqueueState.buffered.size());
  if (!inqueueState.buffered.empty()) {
    // We can enqueue more.
    const auto &newRequest = inqueueState.buffered.front();
    this->enqueue(newRequest, inqueueState.inqueueRequests);
    inqueueState.buffered.pop_front();
    this->totalBufferedRequests--;
  } else if (inqueueState.inqueueRequests == 0) {
    LLC_SLICE_DPRINTF(sliceId, "[ReqBuffer] Released.\n");
    this->inqueueStreamMap.erase(inqueueIter);
  }
}

StreamRequestBuffer::InqueueStreamMapIter
StreamRequestBuffer::getOrInitInqueueState(const RequestPtr &request) {
  const auto &sliceId = request->m_sliceIds.singleSliceId();
  const auto &dynStreamId = sliceId.getDynStreamId();
  auto inqueueIter = this->inqueueStreamMap.find(dynStreamId);
  if (inqueueIter == this->inqueueStreamMap.end()) {
    inqueueIter = this->inqueueStreamMap
                      .emplace(std::piecewise_construct,
                               std::forward_as_tuple(dynStreamId),
                               std::forward_as_tuple())
                      .first;
  }
  return inqueueIter;
}

void StreamRequestBuffer::enqueue(const RequestPtr &request,
                                  int &inqueueRequests) {
  const auto &sliceId = request->m_sliceIds.singleSliceId();
  this->outBuffer->enqueue(request, this->controller->clockEdge(),
                           this->controller->cyclesToTicks(this->latency));
  inqueueRequests++;
  LLC_SLICE_DPRINTF(
      sliceId, "[ReqBuffer] Enqueued into Out MessageBuffer. Inqueue %d.\n",
      inqueueRequests);
}

StreamRequestBuffer::MulticastGroupId
StreamRequestBuffer::getMulticastGroupId(const RequestPtr &request) const {
  /**
   * Only valid for requests with single destination.
   */
  if (request->getDestination().count() != 1) {
    return InvalidMulticastGroupId;
  }
  const auto &dest = request->getDestination().singleElement();
  auto multicastGroupId = this->controller->getMulticastGroupId(
      dest.getNum(), this->multicastBankGroupSize);
  return multicastGroupId;
}

StreamRequestBuffer::MulticastGroupInqueueReqMapIter
StreamRequestBuffer::getOrInitMulticastGroupInqueueReq(
    MulticastGroupId groupId) {
  assert(groupId != InvalidMulticastGroupId && "Invalid MulticastGroupId.");
  auto iter = this->multicastGroupToInqueueReqMap.find(groupId);
  if (iter == this->multicastGroupToInqueueReqMap.end()) {
    iter = this->multicastGroupToInqueueReqMap
               .emplace(std::piecewise_construct,
                        std::forward_as_tuple(groupId), std::forward_as_tuple())
               .first;
  }
  return iter;
}

bool StreamRequestBuffer::shouldTryMulticast(const RequestPtr &req) const {
  if (this->maxMulticastReqPerMsg <= 1) {
    // This feature is not enabled.
    return false;
  }
  if (!req->m_sliceIds.isValid()) {
    return false;
  }
  if (req->getType() != CoherenceRequestType_GETU &&
      req->getType() != CoherenceRequestType_GETH &&
      req->getType() != CoherenceRequestType_STREAM_STORE) {
    return false;
  }
  if (this->getMulticastGroupId(req) == InvalidMulticastGroupId) {
    return false;
  }
  return true;
}

bool StreamRequestBuffer::tryMulticast(const RequestPtr &req) {
  /**
   * Request B can be multicast with request A iff.:
   * 1. Same supported request type (STREAM_STORE, GETU, GETH).
   * Notice that the constraint to the same multicast group is already enforced
   * by the map from multicast group id to inqueue messages, and we remove the
   * request from the MulticastMap if it already contains too many requests.
   */

  assert(this->shouldTryMulticast(req) &&
         "Should never try multicast on this req.");

  auto groupId = this->getMulticastGroupId(req);

  auto groupIter = this->getOrInitMulticastGroupInqueueReq(groupId);
  auto &group = groupIter->second;

  auto multicastReqIter = group.begin();
  auto multicastReqEnd = group.end();
  for (; multicastReqIter != multicastReqEnd; ++multicastReqIter) {
    const auto &candidate = *multicastReqIter;
    if (candidate->getType() == req->getType()) {
      // Found it.
      break;
    }
  }

  if (multicastReqIter == multicastReqEnd) {
    return false;
  }

  auto &multicastReq = *multicastReqIter;
  /**
   * If the multicast request is already too fat (many chained requests), we
   * remove it from the MulticastGroup.
   * Erasing this will not cause the message to be released, as the shared
   * pointer is also captured either in the MessageBuffer or InqueueStreamMap.
   */
  int multicastSize = 1;
  MsgPtr prevChainMsg = multicastReq;
  auto chainMsg = multicastReq->getChainMsg();
  while (chainMsg) {
    multicastSize++;
    prevChainMsg = chainMsg;
    chainMsg = chainMsg->getChainMsg();
  }

  /**
   * Chain to the last message to respect the ordering.
   */
  prevChainMsg->chainMsg(req);

  /**
   * Make sure that MessageBuffer won't unchain this message when enqueued,
   * so that StreamEngine can manually unchain it at the destination.
   */
  req->setUnchainWhenEnqueue(false);
  prevChainMsg->setUnchainWhenEnqueue(false);


  LLC_SLICE_DPRINTF_(LLCRubyStreamMulticast,
                     multicastReq->m_sliceIds.firstSliceId(),
                     "[Multicast] MsgType %s ChainLen %d Chaining %s.\n",
                     multicastReq->getType(), multicastSize, req->m_sliceIds);

  if (multicastSize >= this->maxMulticastReqPerMsg) {
    LLC_SLICE_DPRINTF_(LLCRubyStreamMulticast,
                       multicastReq->m_sliceIds.firstSliceId(),
                       "[Multicast] Erased due to MaxMulticastSize.\n");
    group.erase(multicastReqIter);
  }

  return true;
}