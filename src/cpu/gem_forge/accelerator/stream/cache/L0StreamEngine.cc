
#include "L0StreamEngine.hh"

#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/RubyStream.hh"

#define L0SE_DPRINTF(format, args...)                                          \
  DPRINTF(RubyStream, "[L0_SE%d]: " format,                                    \
          this->controller->getMachineID().num, ##args)

#define L0S_DPRINTF(stream, format, args...)                                   \
  DPRINTF(RubyStream, "[L0_SE%d][%lu]: " format,                               \
          this->controller->getMachineID().num, stream.staticId, ##args)

L0StreamEngine::L0StreamEngine(AbstractStreamAwareController *_controller)
    : controller(_controller) {}

L0StreamEngine::~L0StreamEngine() {}

void L0StreamEngine::receiveStreamConfigure(PacketPtr pkt) {
  auto streamConfigureData = *(pkt->getPtr<CacheStreamConfigureData *>());
  L0SE_DPRINTF("Received StreamConfigure %s.\n",
               streamConfigureData->dynamicId.name.c_str());
  // Add to offloaded stream set.
  this->offloadedStreams.insert(streamConfigureData->dynamicId);
  if (streamConfigureData->indirectStream != nullptr) {
    // We have an indirect stream.
    L0SE_DPRINTF("Received StreamConfigure for indirect %s.\n",
                 streamConfigureData->indirectDynamicId.name.c_str());
    this->offloadedStreams.insert(streamConfigureData->indirectDynamicId);
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
  return true;
}

void L0StreamEngine::serveMiss(PacketPtr pkt) {
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  if (streamMemAccess == nullptr) {
    return;
  }
  auto stream = streamMemAccess->getStream();
  const auto &slice = streamMemAccess->getSliceId();
  // L0S_DPRINTF(slice.streamId, "Miss [%lu, %lu).\n", slice.startIdx,
  //             slice.endIdx);
  stream->numMissL0++;
}

StreamMemAccess *
L0StreamEngine::getStreamMemAccessFromPacket(PacketPtr pkt) const {
  if (pkt == nullptr) {
    return nullptr;
  }
  return pkt->findNextSenderState<StreamMemAccess>();
}