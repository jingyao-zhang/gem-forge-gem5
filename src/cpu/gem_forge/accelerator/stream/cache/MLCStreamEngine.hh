#ifndef __CPU_TDG_ACCELERATOR_STREAM_MLC_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_STREAM_MLC_STREAM_ENGINE_H__

/**
 * Stream engine at MLC cache level, which is private and
 * connects to a lower shared cache.
 *
 * This is where it receives the offloaded streams' data.
 */

#include "MLCDynDirectStream.hh"
#include "MLCDynIndirectStream.hh"

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
  void endStream(const DynStreamId &endId, MasterID masterId);
  void receiveStreamData(const ResponseMsg &msg);
  void receiveStreamDataForSingleSlice(const DynStreamSliceId &sliceId,
                                       const DataBlock &dataBlock,
                                       Addr paddrLine);

  bool isStreamRequest(const DynStreamSliceId &slice);
  bool isStreamOffloaded(const DynStreamSliceId &slice);
  bool isStreamCached(const DynStreamSliceId &slice);
  bool receiveOffloadStreamRequest(const DynStreamSliceId &sliceId);
  void receiveOffloadStreamRequestHit(const DynStreamSliceId &sliceId);

  /**
   * Receive a StreamNDC message.
   * Basically handled by MLCStreamNDCController.
   */
  void receiveStreamNDCRequest(PacketPtr pkt);
  void receiveStreamNDCResponse(const ResponseMsg &msg);

  /**
   * Receive a StreamLoopBound TotalTripCount.
   */
  void receiveStreamTotalTripCount(const DynStreamId &streamId,
                                   int64_t totalTripCount, Addr brokenPAddr,
                                   MachineType brokenMachineType);

  /**
   * API to get the MLCDynStream.
   */
  MLCDynStream *getStreamFromDynamicId(const DynStreamId &id);

private:
  AbstractStreamAwareController *controller;
  MessageBuffer *responseToUpperMsgBuffer;
  MessageBuffer *requestToLLCMsgBuffer;
  std::unordered_map<DynStreamId, MLCDynStream *, DynStreamIdHasher>
      idToStreamMap;

  // For sanity check.
  // TODO: Limit the size of this set.
  std::unordered_set<DynStreamId, DynStreamIdHasher> endedStreamDynamicIds;

  MLCDynStream *getMLCDynStreamFromSlice(const DynStreamSliceId &slice) const;

  /**
   * An experimental new feature: handle reuse among streams.
   */
  void computeReuseInformation(CacheStreamConfigureVec &streamConfigs);

  struct ReuseInfo {
    DynStreamId targetStreamId;
    uint64_t targetCutElementIdx;
    uint64_t targetCutLineVAddr;
    ReuseInfo(const DynStreamId &_targetStreamId, uint64_t _targetCutElementIdx,
              uint64_t _targetCutLineVAddr)
        : targetStreamId(_targetStreamId),
          targetCutElementIdx(_targetCutElementIdx),
          targetCutLineVAddr(_targetCutLineVAddr) {}
  };
  std::unordered_map<DynStreamId, ReuseInfo, DynStreamIdHasher> reuseInfoMap;
  std::unordered_map<DynStreamId, ReuseInfo, DynStreamIdHasher>
      reverseReuseInfoMap;
  void reuseSlice(const DynStreamSliceId &sliceId, const DataBlock &dataBlock);

  /**
   * StreamNDCController.
   */
  friend class MLCStreamNDCController;
  std::unique_ptr<MLCStreamNDCController> ndcController;

  /**
   * Send configure/end message to remote SE.
   */
  void sendConfigToRemoteSE(CacheStreamConfigureDataPtr streamConfigureData,
                            MasterID masterId);
};

#endif