
#include "L0StreamEngine.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/RubyStream.hh"

L0StreamEngine::L0StreamEngine(AbstractStreamAwareController *_controller)
    : controller(_controller) {}

L0StreamEngine::~L0StreamEngine() {}

void L0StreamEngine::receiveMiss(PacketPtr pkt) {
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  if (streamMemAccess == nullptr) {
    return;
  }
  // So far let's bypass private cache level.
  if (streamMemAccess->getStream()->getStreamType() == "load") {
    streamMemAccess->cacheLevel = 1;
  }
}

int L0StreamEngine::getCacheLevel(PacketPtr pkt) {
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  if (streamMemAccess == nullptr) {
    return 0;
  }
  // If stream float is disabled, then not bypass.
  if (!this->controller->isStreamFloatEnabled()) {
    return 0;
  }
  return streamMemAccess->cacheLevel;
}

void L0StreamEngine::serveMiss(PacketPtr pkt) {
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  if (streamMemAccess == nullptr) {
    return;
  }
  auto stream = streamMemAccess->getStream();
  stream->numMissL0++;
}

StreamMemAccess *
L0StreamEngine::getStreamMemAccessFromPacket(PacketPtr pkt) const {
  if (pkt == nullptr) {
    return nullptr;
  }
  return pkt->findNextSenderState<StreamMemAccess>();
}