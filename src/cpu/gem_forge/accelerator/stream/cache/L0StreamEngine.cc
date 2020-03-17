
#include "L0StreamEngine.hh"

#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/L0RubyStream.hh"

#define L0SE_DPRINTF(format, args...)                                          \
  DPRINTF(L0RubyStream, "[L0_SE%d]: " format,                                  \
          this->controller->getMachineID().num, ##args)

#define L0_STREAM_DPRINTF(streamId, format, args...)                           \
  DPRINTF(L0RubyStream, "[L0_SE%d][%lu-%d]: " format,                          \
          this->controller->getMachineID().num, (streamId).staticId,           \
          (streamId).streamInstance, ##args)

#define L0_ELEMENT_DPRINTF(streamId, lhsElementIdx, numElements, format,       \
                           args...)                                            \
  DPRINTF(L0RubyStream, "[L0_SE%d][%lu-%d][%lu, +%d): " format,                \
          this->controller->getMachineID().num, (streamId).staticId,           \
          (streamId).streamInstance, lhsElementIdx, numElements, ##args)

L0StreamEngine::L0StreamEngine(AbstractStreamAwareController *_controller)
    : controller(_controller) {}

L0StreamEngine::~L0StreamEngine() {}

void L0StreamEngine::receiveStreamConfigure(PacketPtr pkt) {
  auto streamConfigs = *(pkt->getPtr<CacheStreamConfigureVec *>());
  for (auto streamConfigureData : *streamConfigs) {
    L0SE_DPRINTF("Received StreamConfigure %s.\n",
                 streamConfigureData->dynamicId.streamName);
    // Add to offloaded stream set.
    assert(!streamConfigureData->isOneIterationBehind &&
           "Only indirect stream can be one iteration behind.");
    this->offloadedStreams.emplace(
        streamConfigureData->dynamicId,
        new L0DynamicStream(streamConfigureData->dynamicId,
                            streamConfigureData));
    for (auto &indirectStreamConfig : streamConfigureData->indirectStreams) {
      // We have an indirect stream.
      L0SE_DPRINTF("Received StreamConfigure for indirect %s.\n",
                   indirectStreamConfig->dynamicId.streamName);
      this->offloadedStreams.emplace(
          indirectStreamConfig->dynamicId,
          new L0DynamicStream(
              streamConfigureData->dynamicId /* RootDynamicStreamId. */,
              indirectStreamConfig.get()));
    }
  }
}

void L0StreamEngine::receiveStreamEnd(PacketPtr pkt) {
  auto endDynamicStreamId = *(pkt->getPtr<DynamicStreamId *>());
  L0_STREAM_DPRINTF(*endDynamicStreamId, "Received StreamEnd.\n");

  auto rootStreamIter = this->offloadedStreams.find(*endDynamicStreamId);
  assert(rootStreamIter != this->offloadedStreams.end() &&
         "Failed to find the ending root stream.");

  // End all streams with the correct root stream id (indirect streams).
  for (auto streamIter = this->offloadedStreams.begin(),
            streamEnd = this->offloadedStreams.end();
       streamIter != streamEnd;) {
    auto stream = streamIter->second;
    if (stream->getRootDynamicStreamId() == (*endDynamicStreamId)) {
      /**
       * ? Can we release right now?
       * Let's keep the ended id for sanity check purpose.
       */
      delete stream;
      streamIter->second = nullptr;
      streamIter = this->offloadedStreams.erase(streamIter);
    } else {
      ++streamIter;
    }
  }
}

bool L0StreamEngine::isStreamAccess(PacketPtr pkt) const {
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  if (streamMemAccess == nullptr) {
    return false;
  }
  // So far let's only consider offloaded stream.
  const auto &dynamicId = streamMemAccess->getDynamicStreamId();
  auto streamIter = this->offloadedStreams.find(dynamicId);
  if (streamIter == this->offloadedStreams.end()) {
    // Failed to find the offloaded stream.
    return false;
  }
  auto stream = streamIter->second;
  // If this is a PseudoOffload stream, we still treat it as normal request.
  if (stream->getIsPseudoOffload()) {
    return false;
  }
  // Check if this is an indirect stream one iteration behind.
  if (stream->getIsOneIterationBehind()) {
    auto sliceId = this->getSliceId(pkt);
    assert(sliceId.getNumElements() == 1 &&
           "Never merge elements for indirect stream one iteration behind.");
    if (sliceId.lhsElementIdx == 0) {
      // Ignore the first stream element.
      return false;
    }
    L0_ELEMENT_DPRINTF(sliceId.streamId, sliceId.lhsElementIdx,
                       sliceId.getNumElements(), "Is stream access.\n");
  }
  return true;
}

DynamicStreamSliceId L0StreamEngine::getSliceId(PacketPtr pkt) const {
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  if (streamMemAccess == nullptr) {
    return DynamicStreamSliceId();
  }
  return streamMemAccess->getSliceId();
}

bool L0StreamEngine::shouldCache(PacketPtr pkt) {
  assert(this->isStreamAccess(pkt) && "Should only handle stream access.");
  if (!this->controller->isStreamFloatEnabled()) {
    return true;
  }
  return false;
}

bool L0StreamEngine::shouldForward(PacketPtr pkt) {
  assert(this->isStreamAccess(pkt) && "Should only handle stream access.");
  if (!this->controller->isStreamFloatEnabled()) {
    return false;
  }
  auto slice = this->getSliceId(pkt);
  L0_ELEMENT_DPRINTF(slice.streamId, slice.lhsElementIdx,
                     slice.rhsElementIdx - slice.lhsElementIdx,
                     "Forward hit.\n");
  return true;
}

StreamMemAccess *
L0StreamEngine::getStreamMemAccessFromPacket(PacketPtr pkt) const {
  if (pkt == nullptr) {
    return nullptr;
  }
  return pkt->findNextSenderState<StreamMemAccess>();
}
