#ifndef __CPU_TDG_ACCELERATOR_STREAM_MLC_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_STREAM_MLC_STREAM_ENGINE_H__

/**
 * Stream engine at MLC cache level, which is private and
 * connects to a lower shared cache.
 *
 * This is where it receives the offloaded streams' data.
 */

#include "MLCDynamicDirectStream.hh"
#include "MLCDynamicIndirectStream.hh"

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

  /**
   * Receive a StreamConfig message and configurea all streams.
   */
  void receiveStreamConfigure(PacketPtr pkt);
  /**
   * Configure a single stream.
   * It will insert an configure message into the message buffer to configure
   * the correct LLC bank.
   * In case the first element's virtual address faulted, the MLC StreamEngine
   * will return physical address that maps to the LLC bank of this tile.
   */
  void configureStream(CacheStreamConfigureData *streamConfigureData,
                       MasterID masterId);
  /**
   * Receive a StreamEnd message.
   * The difference between StreamConfigure and StreamEnd message
   * is that the first one already knows the destination LLC bank from the core
   * StreamEngine, while the later one has to rely on the MLC StreamEngine as it
   * has the flow control information and knows where the stream is.
   * @return The phyiscal line address where the LLC stream is.
   */
  Addr receiveStreamEnd(PacketPtr pkt);
  void receiveStreamData(const ResponseMsg &msg);

  bool isStreamRequest(const DynamicStreamSliceId &slice);
  bool isStreamOffloaded(const DynamicStreamSliceId &slice);
  bool isStreamCached(const DynamicStreamSliceId &slice);
  bool receiveOffloadStreamRequest(const DynamicStreamSliceId &sliceId);
  void receiveOffloadStreamRequestHit(const DynamicStreamSliceId &sliceId);

private:
  AbstractStreamAwareController *controller;
  MessageBuffer *responseToUpperMsgBuffer;
  MessageBuffer *requestToLLCMsgBuffer;
  std::unordered_map<DynamicStreamId, MLCDynamicStream *, DynamicStreamIdHasher>
      idToStreamMap;
  std::list<MLCDynamicStream *> streams;

  // For sanity check.
  // TODO: Limit the size of this set.
  std::unordered_set<DynamicStreamId, DynamicStreamIdHasher>
      endedStreamDynamicIds;

  MLCDynamicStream *
  getMLCDynamicStreamFromSlice(const DynamicStreamSliceId &slice) const;

  MachineID mapPAddrToLLCBank(Addr paddr) const;
};

#endif