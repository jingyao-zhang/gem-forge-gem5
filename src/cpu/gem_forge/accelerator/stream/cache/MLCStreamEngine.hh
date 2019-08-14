#ifndef __CPU_TDG_ACCELERATOR_STREAM_MLC_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_STREAM_MLC_STREAM_ENGINE_H__

/**
 * Stream engine at MLC cache level, which is private and
 * connects to a lower shared cache.
 *
 * This is where it receives the offloaded streams' data.
 */

#include "MLCDynamicStream.hh"

#include "mem/packet.hh"

#include <unordered_map>

class StreamMemAccess;
class MessageBuffer;

/************************************************************************
 * How a stream-aware MLC processes an incoming request from upper level.
 *
 * isStreamRequest? ------> N, handled as normal request.
 * hitInCache?      ------> Y, response & notify stream engine.
 * isOffloaded?     ------> N, isCached? ------> Y, handled as normal request.
 *                                       ------> N, handled as uncached request.
 * isDataReady?     ------> Y, send response.
 */
class MLCStreamEngine {
public:
  MLCStreamEngine(AbstractStreamAwareController *_controller,
                  MessageBuffer *_responseToUpperMsgBuffer,
                  MessageBuffer *_requestToLLCMsgBuffer);
  ~MLCStreamEngine();

  void receiveStreamConfigure(PacketPtr pkt);
  void receiveStreamData(const ResponseMsg &msg);

  void serveMiss(const DynamicStreamSliceId &slice);

  bool isStreamRequest(const DynamicStreamSliceId &slice);
  bool isStreamOffloaded(const DynamicStreamSliceId &slice);
  bool isStreamCached(const DynamicStreamSliceId &slice);
  bool receiveOffloadStreamRequest(const DynamicStreamSliceId &slice);
  void receiveOffloadStreamRequestHit(const DynamicStreamSliceId &slice);

private:
  AbstractStreamAwareController *controller;
  MessageBuffer *responseToUpperMsgBuffer;
  MessageBuffer *requestToLLCMsgBuffer;
  std::unordered_map<DynamicStreamId, MLCDynamicStream *, DynamicStreamIdHasher>
      idToStreamMap;
  std::list<MLCDynamicStream *> streams;

  MLCDynamicStream *
  getMLCDynamicStreamFromSlice(const DynamicStreamSliceId &slice) const;
};

#endif