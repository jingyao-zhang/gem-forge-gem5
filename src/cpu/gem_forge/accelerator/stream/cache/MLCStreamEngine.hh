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
#include "mem/ruby/common/Consumer.hh"

#include <unordered_map>

class MLCStreamNDCController;
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
class MLCStreamEngine : public Consumer {
public:
  MLCStreamEngine(AbstractStreamAwareController *_controller,
                  MessageBuffer *_responseToUpperMsgBuffer,
                  MessageBuffer *_requestToLLCMsgBuffer);
  ~MLCStreamEngine();

  void wakeup() override;
  void print(std::ostream &out) const override;

  /**
   * Receive a StreamConfig message and configure all streams.
   */
  void receiveStreamConfigure(PacketPtr pkt);
  /**
   * Configure a single stream.
   * It will insert an configure message into the message buffer to configure
   * the correct LLC bank.
   * In case the first element's virtual address faulted, the MLC StreamEngine
   * will return physical address that maps to the LLC bank of this tile.
   */
  void configureStream(CacheStreamConfigureDataPtr streamConfigureData,
                       MasterID masterId);
  /**
   * Receive a StreamEnd message and end all streams.
   */
  void receiveStreamEnd(PacketPtr pkt);
  /**
   * Receive a StreamEnd message.
   * The difference between StreamConfigure and StreamEnd message
   * is that the first one already knows the destination LLC bank from the core
   * StreamEngine, while the later one has to rely on the MLC StreamEngine as it
   * has the flow control information and knows where the stream is.
   * It will terminate the stream and send a EndPacket to the LLC bank.
   */
  void endStream(const DynamicStreamId &endId, MasterID masterId);
  void receiveStreamData(const ResponseMsg &msg);
  void receiveStreamDataForSingleSlice(const DynamicStreamSliceId &sliceId,
                                       const DataBlock &dataBlock,
                                       Addr paddrLine);

  bool isStreamRequest(const DynamicStreamSliceId &slice);
  bool isStreamOffloaded(const DynamicStreamSliceId &slice);
  bool isStreamCached(const DynamicStreamSliceId &slice);
  bool receiveOffloadStreamRequest(const DynamicStreamSliceId &sliceId);
  void receiveOffloadStreamRequestHit(const DynamicStreamSliceId &sliceId);

  /**
   * Receive a StreamNDC message.
   * Basically handled by MLCStreamNDCController.
   */
  void receiveStreamNDCRequest(PacketPtr pkt);
  void receiveStreamNDCResponse(const ResponseMsg &msg);

  /**
   * Receive a StreamLoopBound TotalTripCount.
   */
  void receiveStreamTotalTripCount(const DynamicStreamId &streamId,
                                   int64_t totalTripCount, Addr brokenPAddr);

  /**
   * API to get the MLCDynamicStream.
   */
  MLCDynamicStream *getStreamFromDynamicId(const DynamicStreamId &id);

private:
  AbstractStreamAwareController *controller;
  MessageBuffer *responseToUpperMsgBuffer;
  MessageBuffer *requestToLLCMsgBuffer;
  std::unordered_map<DynamicStreamId, MLCDynamicStream *, DynamicStreamIdHasher>
      idToStreamMap;

  // For sanity check.
  // TODO: Limit the size of this set.
  std::unordered_set<DynamicStreamId, DynamicStreamIdHasher>
      endedStreamDynamicIds;

  MLCDynamicStream *
  getMLCDynamicStreamFromSlice(const DynamicStreamSliceId &slice) const;

  MachineID mapPAddrToLLCBank(Addr paddr) const;

  /**
   * An experimental new feature: handle reuse among streams.
   */
  void computeReuseInformation(CacheStreamConfigureVec &streamConfigs);

  struct ReuseInfo {
    DynamicStreamId targetStreamId;
    uint64_t targetCutElementIdx;
    uint64_t targetCutLineVAddr;
    ReuseInfo(const DynamicStreamId &_targetStreamId,
              uint64_t _targetCutElementIdx, uint64_t _targetCutLineVAddr)
        : targetStreamId(_targetStreamId),
          targetCutElementIdx(_targetCutElementIdx),
          targetCutLineVAddr(_targetCutLineVAddr) {}
  };
  std::unordered_map<DynamicStreamId, ReuseInfo, DynamicStreamIdHasher>
      reuseInfoMap;
  std::unordered_map<DynamicStreamId, ReuseInfo, DynamicStreamIdHasher>
      reverseReuseInfoMap;
  void reuseSlice(const DynamicStreamSliceId &sliceId,
                  const DataBlock &dataBlock);

  /**
   * StreamNDCController.
   */
  friend class MLCStreamNDCController;
  std::unique_ptr<MLCStreamNDCController> ndcController;
};

#endif