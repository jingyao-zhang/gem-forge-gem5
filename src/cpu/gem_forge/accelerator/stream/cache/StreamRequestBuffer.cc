#include "StreamRequestBuffer.hh"

#include "debug/LLCRubyStreamBase.hh"
#define DEBUG_TYPE LLCRubyStreamBase
#include "../stream_log.hh"

StreamRequestBuffer::StreamRequestBuffer(
    AbstractStreamAwareController *_controller, MessageBuffer *_outBuffer,
    Cycles _latency, int _maxInqueueRequestsPerStream)
    : controller(_controller), outBuffer(_outBuffer), latency(_latency),
      maxInqueueRequestsPerStream(_maxInqueueRequestsPerStream) {
  this->outBuffer->registerDequeueCallbackOnMsg(
      [this](MsgPtr msg) -> void { this->dequeue(msg); });
}

void StreamRequestBuffer::pushRequest(RequestPtr request) {
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