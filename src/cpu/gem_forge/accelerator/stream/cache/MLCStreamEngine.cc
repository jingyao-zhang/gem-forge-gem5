
#include "MLCStreamEngine.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/RubyStream.hh"

MLCStreamEngine::MLCStreamEngine(AbstractStreamAwareController *_controller,
                                 MessageBuffer *_responseToUpperMsgBuffer,
                                 MessageBuffer *_requestToLLCMsgBuffer)
    : controller(_controller),
      responseToUpperMsgBuffer(_responseToUpperMsgBuffer),
      requestToLLCMsgBuffer(_requestToLLCMsgBuffer) {}

MLCStreamEngine::~MLCStreamEngine() {
  for (auto &stream : this->streams) {
    delete stream;
    stream = nullptr;
  }
  this->streams.clear();
}

void MLCStreamEngine::receiveStreamConfigure(PacketPtr pkt) {
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream configure when stream float is disabled.\n");
  auto streamConfigureData = *(pkt->getPtr<CacheStreamConfigureData *>());
  DPRINTF(RubyStream, "MLCStreamEngine: Received StreamConfigure\n");
  // Create the stream.
  this->streams.emplace_back(new MLCDynamicStream(
      streamConfigureData, this->controller, this->responseToUpperMsgBuffer,
      this->requestToLLCMsgBuffer));
}

void MLCStreamEngine::receiveStreamData(const ResponseMsg &msg) {
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream data when stream float is disabled.\n");
  const auto &streamMeta = msg.m_streamMeta;
  assert(streamMeta.m_valid && "Invalid stream meta-data for stream data.");
  auto stream = reinterpret_cast<Stream *>(streamMeta.m_stream);
  for (auto configuredStream : this->streams) {
    if (configuredStream->getStaticStream() == stream) {
      // Found the stream.
      configuredStream->receiveStreamData(msg);
      return;
    }
  }
  assert(false && "Failed to find configured stream for stream data.");
}

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

bool MLCStreamEngine::isStreamRequest(PacketPtr pkt) {
  if (!this->controller->isStreamFloatEnabled()) {
    return false;
  }
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  return streamMemAccess != nullptr;
}

bool MLCStreamEngine::isStreamOffloaded(PacketPtr pkt) {
  assert(this->isStreamRequest(pkt) && "Should be a stream pkt.");
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  auto staticStream = streamMemAccess->getStream();
  // So far always offload for load stream.
  if (staticStream->getStreamType() == "load") {
    return true;
  }
  return false;
}

bool MLCStreamEngine::isStreamCached(PacketPtr pkt) {
  assert(this->isStreamRequest(pkt) && "Should be a stream pkt.");
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  auto staticStream = streamMemAccess->getStream();
  // So far do not cache for load stream.
  if (staticStream->getStreamType() == "load") {
    return false;
  }
  return true;
}

bool MLCStreamEngine::receiveOffloadStreamRequest(PacketPtr pkt) {
  assert(this->isStreamOffloaded(pkt) && "Should be an offloaded stream pkt.");
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  auto startIdx = streamMemAccess->element->FIFOIdx.entryIdx;
  auto staticStream = streamMemAccess->getStream();

  // Try to find the dynamic stream.
  for (auto &stream : this->streams) {
    if (stream->getStaticStream() == staticStream) {
      stream->receiveStreamRequest(startIdx);
      return true;
    }
  }

  assert(false && "Failed to find the MLCDynamicStream.");
  return false;
}

void MLCStreamEngine::receiveOffloadStreamRequestHit(PacketPtr pkt) {
  assert(this->isStreamOffloaded(pkt) && "Should be an offloaded stream pkt.");
  auto streamMemAccess = this->getStreamMemAccessFromPacket(pkt);
  auto startIdx = streamMemAccess->element->FIFOIdx.entryIdx;
  auto staticStream = streamMemAccess->getStream();

  // Try to find the dynamic stream.
  for (auto &stream : this->streams) {
    if (stream->getStaticStream() == staticStream) {
      stream->receiveStreamRequestHit(startIdx);
      return;
    }
  }

  assert(false && "Failed to find the MLCDynamicStream.");
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