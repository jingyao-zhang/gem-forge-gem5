
#include "MLCStreamEngine.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStream.hh"

#define MLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(MLCRubyStream, "[MLC_SE%d]: " format,                                \
          this->controller->getMachineID().num, ##args)

#define MLC_STREAM_DPRINTF(streamId, format, args...)                          \
  DPRINTF(MLCRubyStream, "[MLC_SE%d][%llu]: " format,                          \
          this->controller->getMachineID().num, streamId, ##args)

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

Addr MLCStreamEngine::receiveStreamConfigure(PacketPtr pkt) {
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream configure when stream float is disabled.\n");
  auto streamConfigureData = *(pkt->getPtr<CacheStreamConfigureData *>());
  MLC_STREAM_DPRINTF(streamConfigureData->dynamicId.staticId,
                     "Received StreamConfigure, totalTripCount %lu.\n",
                     streamConfigureData->totalTripCount);
  /**
   * Do not release the pkt and streamConfigureData as they should be forwarded
   * to the LLC bank and released there. However, we do need to fix up the
   * initPAddr to our LLC bank if case it is not valid.
   * ! This has to be done before initializing the MLCDynamicStream so that it
   * ! knows the initial llc bank.
   */
  if (!streamConfigureData->initPAddrValid) {
    streamConfigureData->initPAddr = this->controller->getAddressToOurLLC();
    streamConfigureData->initPAddrValid = true;
  }

  // Create the direct stream.
  auto directStream = new MLCDynamicDirectStream(
      streamConfigureData, this->controller, this->responseToUpperMsgBuffer,
      this->requestToLLCMsgBuffer);
  this->idToStreamMap.emplace(directStream->getDynamicStreamId(), directStream);
  // Check if there is indirect stream.
  if (streamConfigureData->indirectStreamConfigure != nullptr) {
    // Let's create an indirect stream.
    auto indirectStream = new MLCDynamicIndirectStream(
        streamConfigureData->indirectStreamConfigure.get(), this->controller,
        this->responseToUpperMsgBuffer, this->requestToLLCMsgBuffer,
        directStream->getDynamicStreamId() /* Root dynamic stream id. */);
    this->idToStreamMap.emplace(indirectStream->getDynamicStreamId(),
                                indirectStream);
    directStream->addIndirectStream(indirectStream);
  }

  return streamConfigureData->initPAddr;
}

Addr MLCStreamEngine::receiveStreamEnd(PacketPtr pkt) {
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream end when stream float is disabled.\n");
  auto endDynamicStreamId = *(pkt->getPtr<DynamicStreamId *>());
  MLC_STREAM_DPRINTF(endDynamicStreamId->staticId, "Received StreamEnd.\n");

  // The PAddr of the llc stream. The cache controller uses this to find which
  // LLC bank to forward this StreamEnd message.
  auto rootStreamIter = this->idToStreamMap.find(*endDynamicStreamId);
  assert(rootStreamIter != this->idToStreamMap.end() &&
         "Failed to find the ending root stream.");
  Addr rootLLCStreamPAddr = rootStreamIter->second->getLLCStreamTailPAddr();

  // End all streams with the correct root stream id (indirect streams).
  for (auto streamIter = this->idToStreamMap.begin(),
            streamEnd = this->idToStreamMap.end();
       streamIter != streamEnd;) {
    auto stream = streamIter->second;
    if (stream->getRootDynamicStreamId() == (*endDynamicStreamId)) {
      /**
       * ? Can we release right now?
       * We need to make sure all the seen request is responded (with dummy
       * data).
       * TODO: In the future, if the core doesn't require to send the request,
       * TODO: we are fine to simply release the stream.
       */
      this->endedStreamDynamicIds.insert(stream->getDynamicStreamId());
      stream->endStream();
      delete stream;
      streamIter->second = nullptr;
      streamIter = this->idToStreamMap.erase(streamIter);
    } else {
      ++streamIter;
    }
  }

  // Do not release the pkt and streamDynamicId as they should be forwarded
  // to the LLC bank and released there.

  return makeLineAddress(rootLLCStreamPAddr);
}

void MLCStreamEngine::receiveStreamData(const ResponseMsg &msg) {
  assert(this->controller->isStreamFloatEnabled() &&
         "Receive stream data when stream float is disabled.\n");
  const auto &sliceId = msg.m_sliceId;
  assert(sliceId.isValid() && "Invalid stream slice id for stream data.");
  for (auto &iter : this->idToStreamMap) {
    if (iter.second->getDynamicStreamId() == sliceId.streamId) {
      // Found the stream.
      iter.second->receiveStreamData(msg);
      return;
    }
  }
  // This is possible if the stream is already ended.
  if (this->endedStreamDynamicIds.count(sliceId.streamId) > 0) {
    // The stream is already ended.
    // Sliently ignore it.
    return;
  }
  panic("Failed to find configured stream for %s.\n", sliceId.streamId);
}

bool MLCStreamEngine::isStreamRequest(const DynamicStreamSliceId &slice) {
  if (!this->controller->isStreamFloatEnabled()) {
    return false;
  }
  if (!slice.isValid()) {
    return false;
  }
  // So far just check if the target stream is configured here.
  auto stream = this->getMLCDynamicStreamFromSlice(slice);
  return stream != nullptr;
}

bool MLCStreamEngine::isStreamOffloaded(const DynamicStreamSliceId &slice) {
  assert(this->isStreamRequest(slice) && "Should be a stream request.");
  auto stream = this->getMLCDynamicStreamFromSlice(slice);
  auto staticStream = stream->getStaticStream();
  // So far always offload for load stream.
  if (staticStream->getStreamType() == "load") {
    return true;
  }
  return false;
}

bool MLCStreamEngine::isStreamCached(const DynamicStreamSliceId &slice) {
  assert(this->isStreamRequest(slice) && "Should be a stream request.");
  auto stream = this->getMLCDynamicStreamFromSlice(slice);
  auto staticStream = stream->getStaticStream();
  // So far do not cache for load stream.
  if (staticStream->getStreamType() == "load") {
    return false;
  }
  return true;
}

bool MLCStreamEngine::receiveOffloadStreamRequest(
    const DynamicStreamSliceId &sliceId) {
  assert(this->isStreamOffloaded(sliceId) &&
         "Should be an offloaded stream request.");
  auto stream = this->getMLCDynamicStreamFromSlice(sliceId);
  stream->receiveStreamRequest(sliceId);
  return true;
}

void MLCStreamEngine::receiveOffloadStreamRequestHit(
    const DynamicStreamSliceId &sliceId) {
  assert(this->isStreamOffloaded(sliceId) &&
         "Should be an offloaded stream request.");
  auto stream = this->getMLCDynamicStreamFromSlice(sliceId);
  stream->receiveStreamRequestHit(sliceId);
}

MLCDynamicStream *MLCStreamEngine::getMLCDynamicStreamFromSlice(
    const DynamicStreamSliceId &slice) const {
  if (!slice.isValid()) {
    return nullptr;
  }
  auto iter = this->idToStreamMap.find(slice.streamId);
  if (iter != this->idToStreamMap.end()) {
    // Ignore it if the slice is not considered valid by the stream.
    if (iter->second->isSliceValid(slice)) {
      return iter->second;
    }
  }
  return nullptr;
}
