
#include "MLCStreamEngine.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/RubyStream.hh"

MLCStreamEngine::MLCStreamEngine(AbstractStreamAwareController *_controller)
    : controller(_controller) {}

MLCStreamEngine::~MLCStreamEngine() {}

void MLCStreamEngine::receiveMiss(PacketPtr pkt) {
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  if (streamMemAccess == nullptr) {
    return;
  }
  // So far let's bypass MLC level.
  if (streamMemAccess->getStream()->getStreamType() == "load") {
    if (streamMemAccess->cacheLevel == 1) {
      // If this is cached here (level 1), I move it lower (level 2).
      streamMemAccess->cacheLevel = 2;
    }
  }
}

int MLCStreamEngine::getCacheLevel(PacketPtr pkt) {
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  if (streamMemAccess == nullptr) {
    return 0;
  }
  return streamMemAccess->cacheLevel;
}

void MLCStreamEngine::serveMiss(PacketPtr pkt) {
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  if (streamMemAccess == nullptr) {
    return;
  }
  auto stream = streamMemAccess->getStream();
  stream->numMissL1++;
}

StreamMemAccess *
MLCStreamEngine::getStreamMemAccessFromPacket(PacketPtr pkt) const {
  if (pkt == nullptr) {
    return nullptr;
  }
  return pkt->findNextSenderState<StreamMemAccess>();
}