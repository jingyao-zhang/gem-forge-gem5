#ifndef __CPU_GEM_FORGE_MLC_STRAND_MANAGER_HH__
#define __CPU_GEM_FORGE_MLC_STRAND_MANAGER_HH__

#include "MLCStreamEngine.hh"

class MLCStrandManager {
public:
  MLCStrandManager(MLCStreamEngine *_mlcSE);
  ~MLCStrandManager();

  /**
   * Receive a StreamConfig message and configure all streams.
   */
  void receiveStreamConfigure(PacketPtr pkt);

  /**
   * Receive a StreamEnd message and end all streams.
   */
  void receiveStreamEnd(PacketPtr pkt);

  /**
   * Function called to check CoreCommitProgress for RangeSync.
   */
  void checkCoreCommitProgress();

  /**
   * Get CoreSE. Only valid when there are streams configured.
   */
  StreamEngine *getCoreSE() const;

  /**
   * API to get the MLCDynStream.
   */
  MLCDynStream *getStreamFromDynamicId(const DynStreamId &id);
  bool hasConfiguredStreams() const { return !this->strandMap.empty(); }

private:
  MLCStreamEngine *mlcSE;
  AbstractStreamAwareController *controller;

  std::unordered_map<DynStreamId, MLCDynStream *, DynStreamIdHasher> strandMap;

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
   * Send configure message to remote SE.
   */
  void sendConfigToRemoteSE(CacheStreamConfigureDataPtr streamConfigureData,
                            MasterID masterId);
  /**
   * Receive a StreamEnd message.
   * The difference between StreamConfigure and StreamEnd message
   * is that the first one already knows the destination LLC bank from the core
   * StreamEngine, while the later one has to rely on the MLC StreamEngine as it
   * has the flow control information and knows where the stream is.
   * It will terminate the stream and send a EndPacket to the LLC bank.
   */
  void endStream(const DynStreamId &endId, MasterID masterId);
};

#endif