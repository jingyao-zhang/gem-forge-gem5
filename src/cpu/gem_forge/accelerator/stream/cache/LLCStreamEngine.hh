#ifndef __CPU_TDG_ACCELERATOR_LLC_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_LLC_STREAM_ENGINE_H__

#include "LLCDynamicStream.hh"
#include "StreamReuseBuffer.hh"

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
class LLCStreamMigrationController;
class LLCStreamNDCController;
class LLCStreamAtomicLockManager;
class StreamRequestBuffer;

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
  void receiveStreamDataVec(Cycles delayCycles, Addr paddrLine,
                            const DynamicStreamSliceIdVec &sliceIds,
                            const DataBlock &dataBlock,
                            const DataBlock &storeValueBlock);
  void receiveStreamIndirectRequest(const RequestMsg &req);
  void receiveStreamForwardRequest(const RequestMsg &req);
  void notifyStreamRequestMiss(const DynamicStreamSliceIdVec &sliceIds);
  void wakeup() override;
  void print(std::ostream &out) const override;

  int getNumDirectStreams() const;
  int getNumDirectStreamsWithStaticId(const DynamicStreamId &dynStreamId) const;

  int curRemoteBank() const;
  MachineType myMachineType() const;
  const char *curRemoteMachineType() const;

  Cycles curCycle() const { return this->controller->curCycle(); }

  /**
   * StreamNDC support.
   */
  void receiveStreamNDCRequest(PacketPtr pkt);

private:
  friend class LLCDynamicStream;
  friend class LLCStreamElement;
  friend class LLCStreamCommitController;
  friend class LLCStreamNDCController;
  friend class LLCStreamAtomicLockManager;
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
  std::unique_ptr<LLCStreamMigrationController> migrateController;
  std::unique_ptr<LLCStreamNDCController> ndcController;
  std::unique_ptr<LLCStreamAtomicLockManager> atomicLockManager;
  std::unique_ptr<StreamRequestBuffer> indReqBuffer;
  std::unique_ptr<StreamReuseBuffer> reuseBuffer;
  const int issueWidth;
  const int migrateWidth;
  // Threshold to limit maximum number of infly requests.
  const int maxInflyRequests;
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

  /**
   * Since the LLC controller charge the latency when sending out the response,
   * we want to make sure that this latency is correctly charged for responses
   * to the LLC SE. Normally you would create a MessageBuffer from the
   * controller to the LLC SE. However, that may be a overkill. For now I just
   * add a specific queue for this.
   */
  struct IncomingElementDataMsg {
    const Cycles readyCycle;
    const Addr paddrLine;
    const DynamicStreamSliceId sliceId;
    const DataBlock dataBlock;
    const DataBlock storeValueBlock;
    IncomingElementDataMsg(Cycles _readyCycle, Addr _paddrLine,
                           const DynamicStreamSliceId &_sliceId,
                           const DataBlock &_dataBlock,
                           const DataBlock &_storeValueBlock)
        : readyCycle(_readyCycle), paddrLine(_paddrLine), sliceId(_sliceId),
          dataBlock(_dataBlock), storeValueBlock(_storeValueBlock) {}
  };
  std::list<IncomingElementDataMsg> incomingStreamDataQueue;
  void enqueueIncomingStreamDataMsg(Cycles readyCycle, Addr paddrLine,
                                    const DynamicStreamSliceId &sliceId,
                                    const DataBlock &dataBlock,
                                    const DataBlock &storeValueBlock);
  void drainIncomingStreamDataMsg();
  void receiveStreamData(Addr paddrLine, const DynamicStreamSliceId &sliceId,
                         const DataBlock &dataBlock,
                         const DataBlock &storeValueBlock);
  void receiveStoreStreamData(LLCDynamicStreamPtr dynS,
                              const DynamicStreamSliceId &sliceId,
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
                                     LLCStreamElementPtr element);

  /**
   * Issue indirect load stream request.
   */
  void issueIndirectLoadRequest(LLCDynamicStream *dynIS,
                                LLCStreamElementPtr element);

  /**
   * Issue indirect store/atomic request.
   */
  void issueIndirectStoreOrAtomicRequest(LLCDynamicStream *dynIS,
                                         LLCStreamElementPtr element);

  /**
   * Helper function to enqueue a request and start address translation.
   */
  RequestQueueIter enqueueRequest(Stream *S,
                                  const DynamicStreamSliceId &sliceId,
                                  Addr vaddrLine, Addr paddrLine,
                                  MachineType destMachineType,
                                  CoherenceRequestType type);
  void translationCallback(PacketPtr pkt, ThreadContext *tc,
                           RequestQueueIter reqIter);

  /**
   * Helper function to issue stream request to the remote bank.
   */
  void issueStreamRequestToRemoteBank(const LLCStreamRequest &req);

  using ResponseMsgPtr = std::shared_ptr<ResponseMsg>;
  /**
   * Create the stream message to MLC SE.
   * @param payloadSize: the network should model the payload of this size,
   * instead of the dataSize. By default, it is the same as dataSize.
   * This is used for LoadComputeStream, where the effective size is actually
   * smaller.
   */
  ResponseMsgPtr createStreamMsgToMLC(const DynamicStreamSliceId &sliceId,
                                      CoherenceResponseType type,
                                      Addr paddrLine, const uint8_t *data,
                                      int dataSize, int payloadSize,
                                      int lineOffset);
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
   * @param payloadSize: the network should model the payload of this size,
   * instead of the dataSize.
   * This is used for LoadComputeStream, where the effective size is actually
   * smaller.
   */
  void issueStreamDataToMLC(const DynamicStreamSliceId &sliceId, Addr paddrLine,
                            const uint8_t *data, int dataSize, int payloadSize,
                            int lineOffset, bool forceIdea = false);

  /**
   * Send the stream data to streams another LLC bank. Used for SendTo edge.
   * @param payloadSize: the network should model the payload of this size.
   * This is used for LoadComputeStream, where the effective sizes is actually
   * smaller.
   */
  void issueStreamDataToLLC(LLCDynamicStreamPtr stream,
                            const DynamicStreamSliceId &sliceId,
                            const DataBlock &dataBlock,
                            const CacheStreamConfigureDataPtr &recvConfig,
                            int payloadSize);

  /**
   * Set the TotalTripCount in MLC. Used to implement StreamLoopBound.
   */
  void sendOffloadedLoopBoundRetToMLC(LLCDynamicStreamPtr stream,
                                      uint64_t totalTripCount,
                                      Addr brokenPAddr);

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
  void migrateStreamCommit(LLCDynamicStream *stream, Addr paddr,
                           MachineType machineType);

  /**
   * Helper function to map an address to a same level bank.
   */
  MachineID mapPaddrToSameLevelBank(Addr paddr) const;

  /**
   * Check if this address is handled by myself.
   */
  bool isPAddrHandledByMe(Addr paddr, MachineType machineType) const;

  /**
   * Helper function to check if a stream should
   * be migrated.
   */
  bool canMigrateStream(LLCDynamicStream *stream) const;

  /**
   * Helper function to process stream data for indirect/update.
   */
  void triggerIndirectElement(LLCDynamicStreamPtr stream,
                              LLCStreamElementPtr element);
  void triggerUpdate(LLCDynamicStreamPtr dynS, LLCStreamElementPtr element,
                     const DynamicStreamSliceId &sliceId,
                     const DataBlock &storeValueBlock,
                     DataBlock &loadValueBlock, uint32_t &payloadSize);
  void triggerAtomic(LLCDynamicStreamPtr dynS, LLCStreamElementPtr element,
                     const DynamicStreamSliceId &sliceId,
                     const DataBlock &storeValueBlock,
                     DataBlock &loadValueBlock, uint32_t &payloadSize);

  /**
   * API to manages LLCStreamSlices.
   * Slices are allocated from LLCDynamicStream and now managed by each
   * LLCStreamEngine.
   */
  using SliceList = std::list<LLCStreamSlicePtr>;
  LLCStreamSlicePtr allocateSlice(LLCDynamicStreamPtr dynS);
  LLCStreamSlicePtr tryGetSlice(const DynamicStreamSliceId &sliceId);
  SliceList::iterator releaseSlice(SliceList::iterator sliceIter);
  void processSlices();
  SliceList::iterator processSlice(SliceList::iterator sliceIter);
  void processLoadComputeSlice(LLCDynamicStreamPtr dynS,
                               LLCStreamSlicePtr slice);
  void processAtomicOrUpdateSlice(LLCDynamicStreamPtr dynS,
                                  const DynamicStreamSliceId &sliceId,
                                  const DataBlock &storeValueBlock);

  /**
   * Handle DirectUpdateSlice with the computation latency modelled.
   */
  bool tryProcessDirectUpdateSlice(LLCDynamicStreamPtr dynS,
                                   LLCStreamSlicePtr slice);
  bool tryPostProcessDirectUpdateSlice(LLCDynamicStreamPtr dynS,
                                       LLCStreamSlicePtr slice);
  void postProcessDirectUpdateSlice(LLCDynamicStreamPtr dynS,
                                    const DynamicStreamSliceId &sliceId);

  SliceList allocatedSlices;

  /**
   * Perform store to the BackingStorage.
   */
  void performStore(Addr paddr, int size, const uint8_t *value);

  /**
   * Create the atomic packet.
   */
  PacketPtr createAtomicPacket(Addr vaddr, Addr paddr, int size,
                               std::unique_ptr<StreamAtomicOp> atomicOp);

  /**
   * Perform AtomicRMWStream to the BackingStorage.
   * @return <LoadedValue, MemoryModified>
   */
  std::pair<uint64_t, bool>
  performStreamAtomicOp(LLCDynamicStreamPtr dynS, LLCStreamElementPtr element,
                        Addr elementPAddr, const DynamicStreamSliceId &sliceId);

  /**
   * Process the StreamForward request.
   */
  void processStreamForwardRequest(const RequestMsg &req);

  /**
   * Check if this is the second request to lock the indirect atomic, if so
   * process it.
   * @return whether this message is processed.
   */
  bool tryToProcessIndirectAtomicUnlockReq(const RequestMsg &req);

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
  void recordComputationMicroOps(Stream *S);
  void startComputation();
  void completeComputation();

  void incrementIssueSlice(StreamStatistic &statistic);
};

#endif