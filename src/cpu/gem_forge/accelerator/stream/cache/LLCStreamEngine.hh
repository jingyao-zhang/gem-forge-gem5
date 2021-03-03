#ifndef __CPU_TDG_ACCELERATOR_LLC_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_LLC_STREAM_ENGINE_H__

#include "LLCDynamicStream.hh"

#include "cpu/gem_forge/accelerator/stream/stream_translation_buffer.hh"

// Generate by slicc.
#include "mem/ruby/protocol/RequestMsg.hh"
#include "mem/ruby/protocol/ResponseMsg.hh"

#include "mem/ruby/common/Consumer.hh"

/**
 * Derive from Consumer to schedule wakeup event.
 */
#include <list>
#include <map>
#include <memory>
#include <set>

class AbstractStreamAwareController;
class MessageBuffer;
class LLCStreamCommitController;

class LLCStreamEngine : public Consumer {
public:
  LLCStreamEngine(AbstractStreamAwareController *_controller,
                  MessageBuffer *_streamMigrateMsgBuffer,
                  MessageBuffer *_streamIssueMsgBuffer,
                  MessageBuffer *_streamIndirectIssueMsgBuffer,
                  MessageBuffer *_streamResponseMsgBuffer);
  ~LLCStreamEngine() override;

  void receiveStreamConfigure(PacketPtr pkt);
  void receiveStreamEnd(PacketPtr pkt);
  void receiveStreamMigrate(LLCDynamicStreamPtr stream, bool isCommit);
  void receiveStreamFlow(const DynamicStreamSliceId &sliceId);
  void receiveStreamCommit(const DynamicStreamSliceId &sliceId);
  void receiveStreamElementDataVec(Cycles delayCycles,
                                   const DynamicStreamSliceIdVec &sliceIds,
                                   const DataBlock &dataBlock,
                                   const DataBlock &storeValueBlock);
  void receiveStreamIndirectRequest(const RequestMsg &req);
  void receiveStreamForwardRequest(const RequestMsg &req);
  void wakeup() override;
  void print(std::ostream &out) const override;

private:
  friend class LLCDynamicStream;
  friend class LLCStreamElement;
  friend class LLCStreamCommitController;
  AbstractStreamAwareController *controller;
  // Out going stream migrate buffer.
  MessageBuffer *streamMigrateMsgBuffer;
  // Issue stream request here at the local bank.
  MessageBuffer *streamIssueMsgBuffer;
  // Issue stream request to a remote bank.
  MessageBuffer *streamIndirectIssueMsgBuffer;
  // Send response to MLC.
  MessageBuffer *streamResponseMsgBuffer;
  // Stream commit controller.
  std::unique_ptr<LLCStreamCommitController> commitController;
  const int issueWidth;
  const int migrateWidth;
  // Threshold to limit maximum number of infly requests.
  const int maxInflyRequests;
  const int maxInflyRequestsPerStream;
  // Threshold to limit maximum number of requests in queue;
  const int maxInqueueRequests;

  using StreamSet = std::set<LLCDynamicStreamPtr>;
  using StreamVec = std::vector<LLCDynamicStreamPtr>;
  using StreamList = std::list<LLCDynamicStreamPtr>;
  using StreamListIter = StreamList::iterator;
  StreamList streams;
  /**
   * Streams waiting to be migrated to other LLC bank.
   */
  StreamList migratingStreams;

  int curLLCBank() const;

  /**
   * Since the LLC controller charge the latency when sending out the response,
   * we want to make sure that this latency is correctly charged for responses
   * to the LLC SE. Normally you would create a MessageBuffer from the
   * controller to the LLC SE. However, that may be a overkill. For now I just
   * add a specific queue for this.
   */
  struct IncomingElementDataMsg {
    const Cycles readyCycle;
    const DynamicStreamSliceId sliceId;
    const DataBlock dataBlock;
    const DataBlock storeValueBlock;
    IncomingElementDataMsg(Cycles _readyCycle,
                           const DynamicStreamSliceId &_sliceId,
                           const DataBlock &_dataBlock,
                           const DataBlock &_storeValueBlock)
        : readyCycle(_readyCycle), sliceId(_sliceId), dataBlock(_dataBlock),
          storeValueBlock(_storeValueBlock) {}
  };
  std::list<IncomingElementDataMsg> incomingElementDataQueue;
  void enqueueIncomingElementDataMsg(Cycles readyCycle,
                                     const DynamicStreamSliceId &sliceId,
                                     const DataBlock &dataBlock,
                                     const DataBlock &storeValueBlock);
  void drainIncomingElementDataMsg();
  void receiveStreamElementData(const DynamicStreamSliceId &sliceId,
                                const DataBlock &dataBlock,
                                const DataBlock &storeValueBlock);

  /**
   * Bidirectionaly map between streams that are identical but
   * to different cores.
   */
  std::map<LLCDynamicStreamPtr, StreamVec> multicastStreamMap;

  /**
   * Buffered stream flow message waiting for the stream to migrate here.
   */
  std::list<DynamicStreamSliceId> pendingStreamFlowControlMsgs;

  /**
   * Buffered stream end message waiting for the stream to migrate here.
   */
  std::unordered_set<DynamicStreamId, DynamicStreamIdHasher>
      pendingStreamEndMsgs;

  /**
   * Buffered stream forward message waiting for the stream to migrate here.
   */
  std::list<RequestMsg> pendingStreamForwardMsgs;

  /**
   * Hold the request queue.
   */
  std::list<LLCStreamRequest> requestQueue;

  /**
   * Hold the request in translation. The request should be in
   * requestQueue.
   */
  using RequestQueueIter = std::list<LLCStreamRequest>::iterator;
  std::unique_ptr<StreamTranslationBuffer<RequestQueueIter>> translationBuffer =
      nullptr;

  /**
   * TranslationBuffer can only be initialized after start up as it
   * requires the TLB.
   */
  void initializeTranslationBuffer();

  /**
   * Check if two streams can be merged into a multicast stream.
   */
  bool canMergeAsMulticast(LLCDynamicStreamPtr dynSA,
                           LLCDynamicStreamPtr dynSB) const;
  void addStreamToMulticastTable(LLCDynamicStreamPtr dynS);
  void removeStreamFromMulticastTable(LLCDynamicStreamPtr dynS);
  bool hasMergedAsMulticast(LLCDynamicStreamPtr dynS) const;
  StreamVec &getMulticastGroup(LLCDynamicStreamPtr dynS);
  const StreamVec &getMulticastGroup(LLCDynamicStreamPtr dynS) const;

  /**
   * Check if the stream can issue by MulticastPolicy.
   */
  bool canIssueByMulticastPolicy(LLCDynamicStreamPtr dynS) const;

  /**
   * Sort the multicast group s.t. behind streams comes first.
   */
  void sortMulticastGroup(StreamVec &group) const;
  /**
   * Given a request, we check if we can have multicast slice.
   */
  void generateMulticastRequest(RequestQueueIter reqIter,
                                LLCDynamicStreamPtr dynS);

  /**
   * Process stream flow control messages and distribute
   * them to the coresponding stream.
   */
  void processStreamFlowControlMsg();

  /**
   * Issue streams in a round-robin way.
   */
  void issueStreams();

  /**
   * Find a stream within this S and its indirect streams ready to issue.
   * @return nullptr if not found.
   */
  LLCDynamicStreamPtr findStreamReadyToIssue(LLCDynamicStreamPtr dynS);
  LLCDynamicStreamPtr findIndirectStreamReadyToIssue(LLCDynamicStreamPtr dynS);

  /**
   * Issue a DirectStream.
   */
  void issueStreamDirect(LLCDynamicStream *dynS);

  /**
   * Issue the indirect elements for a stream.
   */
  void issueStreamIndirect(LLCDynamicStream *dynIS);

  /**
   * Get the request type for this stream.
   */
  CoherenceRequestType getDirectStreamReqType(LLCDynamicStream *stream) const;

  /**
   * Generate indirect stream request.
   */
  void generateIndirectStreamRequest(LLCDynamicStream *dynIS,
                                     uint64_t elementIdx);

  /**
   * Helper function to enqueue a request and start address translation.
   */
  RequestQueueIter enqueueRequest(GemForgeCPUDelegator *cpuDelegator,
                                  const DynamicStreamSliceId &sliceId,
                                  Addr vaddrLine, Addr paddrLine,
                                  CoherenceRequestType type);
  void translationCallback(PacketPtr pkt, ThreadContext *tc,
                           RequestQueueIter reqIter);

  /**
   * Helper function to issue stream request to the LLC cache bank.
   */
  void issueStreamRequestToLLCBank(const LLCStreamRequest &req);

  using ResponseMsgPtr = std::shared_ptr<ResponseMsg>;
  ResponseMsgPtr createStreamMsgToMLC(const DynamicStreamSliceId &sliceId,
                                      CoherenceResponseType type,
                                      Addr paddrLine, const uint8_t *data,
                                      int size, int lineOffset);
  void issueStreamMsgToMLC(ResponseMsgPtr msg, bool forceIdea = false);

  /**
   * Helper function to issue stream ack back to MLC at request core.
   */
  void issueStreamAckToMLC(const DynamicStreamSliceId &sliceId,
                           bool forceIdea = false);

  /**
   * Helper function to issue StreamDone back to MLC at request core.
   */
  void issueStreamDoneToMLC(const DynamicStreamSliceId &sliceId,
                            bool forceIdea = false);

  /**
   * Helper function to issue stream range back to MLC at request core.
   */
  void issueStreamRangesToMLC();
  void issueStreamRangeToMLC(DynamicStreamAddressRangePtr &range,
                             bool forceIdea = false);

  /**
   * Helper function to issue stream data back to MLC at request core.
   * Mostly used for atomic streams.
   */
  void issueStreamDataToMLC(const DynamicStreamSliceId &sliceId, Addr paddrLine,
                            const uint8_t *data, int size, int lineOffset,
                            bool forceIdea = false);
  /**
   * Send the stream data to streams another LLC bank. Used for SendTo edge.
   */
  void issueStreamDataToLLC(LLCDynamicStreamPtr stream,
                            const DynamicStreamSliceId &sliceId,
                            const DataBlock &dataBlock,
                            const CacheStreamConfigureDataPtr &recvConfig);

  /**
   * Find streams that should be migrated.
   */
  void findMigratingStreams();

  /**
   * Migrate streams.
   */
  void migrateStreams();

  /**
   * Migrate a single stream.
   */
  void migrateStream(LLCDynamicStream *stream);

  /**
   * Migrate a stream's commit head.
   */
  void migrateStreamCommit(LLCDynamicStream *stream, Addr paddr);

  /**
   * Helper function to map an address to a LLC bank.
   */
  MachineID mapPaddrToLLCBank(Addr paddr) const;

  /**
   * Check if this address is handled by myself.
   */
  bool isPAddrHandledByMe(Addr paddr) const;

  /**
   * Helper function to check if a stream should
   * be migrated.
   */
  bool canMigrateStream(LLCDynamicStream *stream) const;

  /**
   * Helper function to process stream data for indirect/update.
   */
  void processStreamDataForIndirectStreams(LLCDynamicStreamPtr stream,
                                           LLCStreamElementPtr element);
  void processStreamDataForUpdate(LLCDynamicStreamPtr stream,
                                  LLCStreamElementPtr element,
                                  const DataBlock &storeValueBlock,
                                  DataBlock &loadValueBlock);
  void updateElementData(LLCDynamicStreamPtr stream, uint64_t elementIdx,
                         uint64_t updateValue);
  /**
   * Perform store to the BackingStorage.
   */
  void performStore(Addr paddr, int size, const uint8_t *value);

  /**
   * Perform AtomicRMWStream to the BackingStorage.
   * Return the loaded value.
   */
  uint64_t performStreamAtomicOp(Addr elementVAddr, Addr elementPAddr,
                                 LLCDynamicStreamPtr stream,
                                 const DynamicStreamSliceId &sliceId);

  /**
   * Try to process the request. It may not be able to process it if
   * the stream lags behind.
   * @return: whether this message is processed.
   */
  bool tryProcessStreamForwardRequest(const RequestMsg &req);

  /**
   * We handle the computation and charge its latency here.
   * The direct stream will also remember the number of incomplete computation
   * from itself (e.g. StoreStream with StoreFunc) and its indirect streams
   * (e.g. Reduction).
   */
  std::list<LLCStreamElementPtr> readyComputations;
  struct InflyComputation {
    LLCStreamElementPtr element;
    StreamValue result;
    Cycles readyCycle;
    InflyComputation(const LLCStreamElementPtr &_element,
                     const StreamValue &_result, Cycles _readyCycle)
        : element(_element), result(_result), readyCycle(_readyCycle) {}
  };
  std::list<InflyComputation> inflyComputations;
  void pushReadyComputation(LLCStreamElementPtr &element);
  void pushInflyComputation(LLCStreamElementPtr &element,
                            const StreamValue &result, Cycles &latency);
  void startComputation();
  void completeComputation();
};

#endif