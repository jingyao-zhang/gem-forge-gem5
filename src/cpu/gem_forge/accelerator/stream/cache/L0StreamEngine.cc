
#include "L0StreamEngine.hh"

#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/RubyStream.hh"

#define L0SE_DPRINTF(format, args...)                                          \
  DPRINTF(RubyStream, "[L0_SE%d]: " format,                                    \
          this->controller->getMachineID().num, ##args)

#define L0_STREAM_DPRINTF(streamId, format, args...)                           \
  DPRINTF(RubyStream, "[L0_SE%d][%lu]: " format,                               \
          this->controller->getMachineID().num, (streamId).staticId, ##args)

#define L0_ELEMENT_DPRINTF(streamId, startIdx, numElements, format, args...)   \
  DPRINTF(RubyStream, "[L0_SE%d][%lu][%lu, +%d): " format,                     \
          this->controller->getMachineID().num, (streamId).staticId, startIdx, \
          numElements, ##args)

L0StreamEngine::L0StreamEngine(AbstractStreamAwareController *_controller)
    : controller(_controller) {}

L0StreamEngine::~L0StreamEngine() {}

void L0StreamEngine::receiveStreamConfigure(PacketPtr pkt) {
  auto streamConfigureData = *(pkt->getPtr<CacheStreamConfigureData *>());
  L0SE_DPRINTF("Received StreamConfigure %s.\n",
               streamConfigureData->dynamicId.name.c_str());
  hack("L0SE configured for %llu.\n", streamConfigureData->dynamicId.streamInstance);
  // Add to offloaded stream set.
  this->offloadedStreams.emplace(
      streamConfigureData->dynamicId,
      new L0DynamicStream(streamConfigureData->dynamicId));
  if (streamConfigureData->indirectStreamConfigure != nullptr) {
    // We have an indirect stream.
    L0SE_DPRINTF(
        "Received StreamConfigure for indirect %s.\n",
        streamConfigureData->indirectStreamConfigure->dynamicId.name.c_str());
    this->offloadedStreams.emplace(
        streamConfigureData->indirectStreamConfigure->dynamicId,
        new L0DynamicStream(
            streamConfigureData->dynamicId /* RootDynamicStreamId. */));
  }
}

void L0StreamEngine::receiveStreamEnd(PacketPtr pkt) {
  auto endDynamicStreamId = *(pkt->getPtr<DynamicStreamId *>());
  L0_STREAM_DPRINTF(*endDynamicStreamId, "Received StreamEnd.\n");
  hack("L0SE end for %llu.\n", endDynamicStreamId->streamInstance);

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
  return this->offloadedStreams.count(dynamicId) != 0;
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
  L0_ELEMENT_DPRINTF(slice.streamId, slice.startIdx,
                     slice.endIdx - slice.startIdx, "Forward hit.\n");
  return true;
}

StreamMemAccess *
L0StreamEngine::getStreamMemAccessFromPacket(PacketPtr pkt) const {
  if (pkt == nullptr) {
    return nullptr;
  }
  return pkt->findNextSenderState<StreamMemAccess>();
}