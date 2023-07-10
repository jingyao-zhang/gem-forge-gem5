#ifndef __CPU_TDG_ACCELERATOR_LLC_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_LLC_STREAM_ENGINE_H__

#include "LLCDynStream.hh"
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

namespace gem5 {

namespace ruby {
class AbstractStreamAwareController;
}
class MessageBuffer;
class LLCStreamCommitController;
class LLCStreamMigrationController;
class LLCStreamNDCController;
class LLCStreamAtomicLockManager;
class StreamRequestBuffer;
class PUMEngine;

class LLCStreamEngine : public ruby::Consumer {
public:
  LLCStreamEngine(ruby::AbstractStreamAwareController *_controller,
                  ruby::MessageBuffer *_streamMigrateMsgBuffer,
                  ruby::MessageBuffer *_streamIssueMsgBuffer,
                  ruby::MessageBuffer *_streamIndirectIssueMsgBuffer,
                  ruby::MessageBuffer *_streamResponseMsgBuffer);
  ~LLCStreamEngine() override;

  void receiveStreamConfigure(PacketPtr pkt);
  void receiveStreamEnd(PacketPtr pkt);
  void receiveStreamMigrate(LLCDynStreamPtr stream, bool isCommit);
  void receiveStreamFlow(const DynStreamSliceId &sliceId);
  void receiveStreamCommit(const DynStreamSliceId &sliceId);
  void receiveStreamDataVecFromCache(Cycles delayCycles, Addr paddrLine,
                                     const DynStreamSliceIdVec &sliceIds,
                                     const ruby::DataBlock &dataBlock,
                                     const ruby::DataBlock &storeValueBlock);
  void receiveStreamIndirectReq(const ruby::RequestMsg &req);
  void receiveStreamIndirectReqImpl(const ruby::RequestMsg &req);
  void receiveStreamFwdReq(const ruby::RequestMsg &req);
  void notifyStreamRequestMiss(const DynStreamSliceIdVec &sliceIds);
  void wakeup() override;
  void print(std::ostream &out) const override;

  void resetStats();

  int getNumDirectStreams() const;
  int getNumNonOverflownDirectStreamsWithStaticId(
      const DynStreamId &dynStreamId) const;

  int curRemoteBank() const;
  ruby::MachineType myMachineType() const;
  const char *curRemoteMachineType() const;

  Cycles curCycle() const { return this->controller->curCycle(); }

  /**
   * StreamNDC support.
   */
  void receiveStreamNDCRequest(PacketPtr pkt);

  /**
   * API wrapper for PUMEngine.
   */
  std::unique_ptr<PUMEngine> &getPUMEngine() { return this->pumEngine; }

  /**
   * Receive the PUM configure.
   */
  void receivePUMConfigure(const ruby::RequestMsg &req);

  /**
   * Receive the PUM data.
   */
  void receivePUMData(const ruby::RequestMsg &req);

private:
  friend class LLCDynStream;
  friend class LLCStreamElement;
  friend class LLCStreamCommitController;
  friend class LLCStreamNDCController;
  friend class LLCStreamAtomicLockManager;
  friend class PUMEngine;
  ruby::AbstractStreamAwareController *controller;
  // Out going stream migrate buffer.
  ruby::MessageBuffer *streamMigrateMsgBuffer;
  // Issue stream request here at the local bank.
  ruby::MessageBuffer *streamIssueMsgBuffer;
  // Issue stream request to a remote bank.
  ruby::MessageBuffer *streamIndirectIssueMsgBuffer;
  // Send response to MLC.
  ruby::MessageBuffer *streamResponseMsgBuffer;
  // Stream commit controller.
  std::unique_ptr<LLCStreamCommitController> commitController;
  std::unique_ptr<LLCStreamMigrationController> migrateController;
  std::unique_ptr<LLCStreamNDCController> ndcController;
  std::unique_ptr<LLCStreamAtomicLockManager> atomicLockManager;
  std::unique_ptr<StreamRequestBuffer> indReqBuffer;
  std::unique_ptr<StreamReuseBuffer> reuseBuffer;
  std::unique_ptr<PUMEngine> pumEngine;
  const int issueWidth;
  const int migrateWidth;
  // Threshold to limit maximum number of infly requests.
  const int maxInflyRequests;
  // Threshold to limit maximum number of requests in queue;
  const int maxInqueueRequests;

  using StreamSet = std::set<LLCDynStreamPtr>;
  using StreamVec = std::vector<LLCDynStreamPtr>;
  using StreamList = std::list<LLCDynStreamPtr>;
  using StreamListIter = StreamList::iterator;
  StreamList streams;

  using StrandIdSet = std::unordered_set<DynStrandId, DynStrandIdHasher>;
  using StrandIdList = std::list<DynStrandId>;
  /**
   * DirectStreams waiting to be issued.
   * This optimization removes DirectStreams that have overflown from IssueList.
   * Also, LLCStreamEngine searches for migrating streams within this IssueList.
   */
  StrandIdList issuingDirStreamList;
  /**
   * IndirectStreams waiting to be issued.
   * Have a list and set at the same time.
   */
  StrandIdList issuingIndStreamList;
  StrandIdSet issuingIndStreamSet;

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
    const DynStreamSliceId sliceId;
    const ruby::DataBlock dataBlock;
    const ruby::DataBlock storeValueBlock;
    IncomingElementDataMsg(Cycles _readyCycle, Addr _paddrLine,
                           const DynStreamSliceId &_sliceId,
                           const ruby::DataBlock &_dataBlock,
                           const ruby::DataBlock &_storeValueBlock)
        : readyCycle(_readyCycle), paddrLine(_paddrLine), sliceId(_sliceId),
          dataBlock(_dataBlock), storeValueBlock(_storeValueBlock) {}
  };
  std::list<IncomingElementDataMsg> incomingStreamDataQueue;
  void enqueueIncomingStreamDataMsg(Cycles readyCycle, Addr paddrLine,
                                    const DynStreamSliceId &sliceId,
                                    const ruby::DataBlock &dataBlock,
                                    const ruby::DataBlock &storeValueBlock);

  void receiveStreamDataVec(Cycles delayCycles, Addr paddrLine,
                            const DynStreamSliceIdVec &sliceIds,
                            const ruby::DataBlock &dataBlock,
                            const ruby::DataBlock &storeValueBlock);

  void receiveStreamDataVecFromReuse(Cycles delayCycles, Addr paddrLine,
                                     const DynStreamSliceIdVec &sliceIds,
                                     const ruby::DataBlock &dataBlock,
                                     const ruby::DataBlock &storeValueBlock) {
    this->receiveStreamDataVec(delayCycles, paddrLine, sliceIds, dataBlock,
                               storeValueBlock);
  }

  void drainIncomingStreamDataMsg();
  void receiveStreamData(Addr paddrLine, const DynStreamSliceId &sliceId,
                         const ruby::DataBlock &dataBlock,
                         const ruby::DataBlock &storeValueBlock);
  void receiveStoreStreamData(LLCDynStreamPtr dynS,
                              const DynStreamSliceId &sliceId,
                              const ruby::DataBlock &storeValueBlock);

  /**
   * Bidirectionaly map between streams that are identical but
   * to different cores.
   */
  std::map<LLCDynStreamPtr, StreamVec> multicastStreamMap;

  /**
   * Buffered stream flow message waiting for the stream to migrate here.
   */
  std::list<DynStreamSliceId> pendingStreamFlowControlMsgs;

  /**
   * Buffered stream end message waiting for the stream to migrate here.
   */
  std::unordered_set<DynStrandId, DynStrandIdHasher> pendingEndStrandIds;

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
  bool canMergeAsMulticast(LLCDynStreamPtr dynSA, LLCDynStreamPtr dynSB) const;
  void addStreamToMulticastTable(LLCDynStreamPtr dynS);
  void removeStreamFromMulticastTable(LLCDynStreamPtr dynS);
  bool hasMergedAsMulticast(LLCDynStreamPtr dynS) const;
  StreamVec &getMulticastGroup(LLCDynStreamPtr dynS);
  const StreamVec &getMulticastGroup(LLCDynStreamPtr dynS) const;

  /**
   * Check if the stream can issue by MulticastPolicy.
   */
  bool canIssueByMulticastPolicy(LLCDynStreamPtr dynS) const;

  /**
   * Sort the multicast group s.t. behind streams comes first.
   */
  void sortMulticastGroup(StreamVec &group) const;
  /**
   * Given a request, we check if we can have multicast slice.
   */
  void generateMulticastRequest(RequestQueueIter reqIter, LLCDynStreamPtr dynS);

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
   * Find a stream ready to issue.
   * @return nullptr if not found.
   */
  LLCDynStreamPtr findStreamReadyToIssue(LLCDynStreamPtr dynS);

  /**
   * Helper function to manage the issuing streams.
   */
  void addIssuingDirDynS(LLCDynStreamPtr dynS);
  StrandIdList::iterator removeIssuingDirDynS(StrandIdList::iterator iter);
  void tryRemoveIssuingDirDynS(LLCDynStreamPtr dynS);
  void addIssuingIndDynS(LLCDynStreamPtr dynIS);
  void removeIssuingIndDynS(StrandIdList::iterator iter);

  void removeDynS(LLCDynStreamPtr dynS);

  /**
   * Issue a DirectStream.
   */
  void issueStreamDirect(LLCDynStream *dynS);

  /**
   * Issue the indirect elements for a stream.
   */
  void issueStreamIndirect(LLCDynStream *dynIS);

  /**
   * Get the request type for this stream.
   */
  ruby::CoherenceRequestType getStreamReqType(LLCDynStream *stream) const;

  /**
   * Generate indirect stream request.
   */
  void generateIndirectStreamReq(LLCDynStream *dynIS, LLCStreamElementPtr elem);

  /**
   * Issue indirect load stream request.
   */
  void issueIndirectLoadReq(LLCDynStream *dynIS, LLCStreamElementPtr elem);

  /**
   * Issue indirect store/atomic request.
   */
  void issueIndirectStoreOrAtomicReq(LLCDynStream *dynIS,
                                     LLCStreamElementPtr elem);

  /**
   * Issue indirect atomic unlock request.
   */
  void issueIndirectAtomicUnlockRequest(LLCDynStream *dynIS,
                                        LLCStreamElementPtr elem);

  /**
   * Helper function to enqueue a request and start address translation.
   */
  RequestQueueIter enqueueRequest(Stream *S, const DynStreamSliceId &sliceId,
                                  Addr vaddrLine, Addr paddrLine,
                                  ruby::MachineType destMachineType,
                                  ruby::CoherenceRequestType type);
  void translationCallback(PacketPtr pkt, ThreadContext *tc,
                           RequestQueueIter reqIter);

  /**
   * Helper function to issue stream request to the remote bank.
   */
  void issueStreamReqToRemoteBank(const LLCStreamRequest &req);

  using ResponseMsgPtr = std::shared_ptr<ruby::ResponseMsg>;
  /**
   * Create the stream message to MLC SE.
   * @param payloadSize: the network should model the payload of this size,
   * instead of the dataSize. By default, it is the same as dataSize.
   * This is used for LoadComputeStream, where the effective size is actually
   * smaller.
   */
  ResponseMsgPtr createStreamMsgToMLC(const DynStreamSliceId &sliceId,
                                      ruby::CoherenceResponseType type,
                                      Addr paddrLine, const uint8_t *data,
                                      int dataSize, int payloadSize,
                                      int lineOffset);
  void issueStreamMsgToMLC(ResponseMsgPtr msg, bool forceIdea = false);

  /**
   * Helper function to issue stream ack back to MLC at request core.
   */
  void issueStreamAckToMLC(const DynStreamSliceId &sliceId,
                           bool forceIdea = false);

  /**
   * Helper function to issue StreamDone back to MLC at request core.
   */
  void issueStreamDoneToMLC(const DynStreamSliceId &sliceId,
                            bool forceIdea = false);

  /**
   * Helper function to issue stream range back to MLC at request core.
   */
  void issueStreamRangesToMLC();
  void issueStreamRangeToMLC(DynStreamAddressRangePtr &range,
                             bool forceIdea = false);

  /**
   * Helper function to issue stream data back to MLC at request core.
   * Mostly used for atomic streams.
   * @param payloadSize: the network should model the payload of this size,
   * instead of the dataSize.
   * This is used for LoadComputeStream, where the effective size is actually
   * smaller.
   */
  void issueStreamDataToMLC(const DynStreamSliceId &sliceId, Addr paddrLine,
                            const uint8_t *data, int dataSize, int payloadSize,
                            int lineOffset, bool forceIdea = false);

  /**
   * Send the stream data to streams another LLC bank. Used for SendTo edge.
   * @param payloadSize: the network should model the payload of this size.
   * This is used for LoadComputeStream, where the effective sizes is actually
   * smaller.
   */
  void issueStreamDataToLLC(
      LLCDynStreamPtr dynS, const DynStreamSliceId &sliceId,
      const ruby::DataBlock &dataBlock,
      const std::vector<CacheStreamConfigureData::DepEdge> &sendToEdges,
      int payloadSize);

  /**
   * Send the NoMigration stream data back to its req. bank. Used to implement
   * NoNSC configuration, in which streams do not float but stays at its own
   * bank.
   * @param payloadSize: the network should model the payload of this size.
   * This is used for LoadComputeStream, where the effective sizes is actually
   * smaller.
   */
  void issueNonMigrateStreamDataToLLC(LLCDynStreamPtr dynS,
                                      const DynStreamSliceId &sliceId,
                                      const ruby::DataBlock &dataBlock,
                                      const ruby::DataBlock &storeValueBlock,
                                      int payloadSize);

  /**
   * Send the stream data to PUM. Used for PUMSendTo edge.
   * @param payloadSize: the network should model the payload of this size.
   * This is used for LoadComputeStream, where the effective sizes is actually
   * smaller.
   */
  void issueStreamDataToPUM(LLCDynStreamPtr dynS,
                            const DynStreamSliceId &sliceId,
                            const ruby::DataBlock &dataBlock,
                            const CacheStreamConfigureData::DepEdge &sendToEdge,
                            int payloadSize);

  /**
   * Send the PUMPrefetch MemStream Data back to LLC.
   */
  void issuePUMPrefetchStreamDataToLLC(LLCDynStreamPtr stream,
                                       const DynStreamSliceId &sliceId,
                                       const ruby::DataBlock &dataBlock);

  /**
   * Notify the MLCPUMManager when a PUMPrefetchStream is done.
   */
  bool tryFinishPUMPrefetchStream(LLCDynStreamPtr dynS,
                                  const DynStreamSliceId &sliceId);

  /**
   * Set the TotalTripCount in MLC. Used to implement StreamLoopBound.
   */
  void sendLoopBoundRetToMLC(LLCDynStreamPtr stream, uint64_t elemIdx,
                             bool broken);

  /**
   * Find streams that should be migrated.
   */
  void findMigratingStreams();

  /**
   * Check if next element is handled here or not.
   *
   */
  bool isNextElemHandledHere(LLCDynStreamPtr dynS) const;

  /**
   * Migrate streams.
   */
  void migrateStreams();

  /**
   * Migrate a single stream.
   */
  void migrateStream(LLCDynStream *stream);

  /**
   * Migrate a stream's commit head.
   */
  void migrateStreamCommit(LLCDynStream *stream, Addr paddr,
                           ruby::MachineType machineType);

  /**
   * Helper function to map an address to a same level bank.
   */
  ruby::MachineID mapPaddrToSameLevelBank(Addr paddr) const;

  /**
   * Check if this address is handled by myself.
   */
  bool isPAddrHandledByMe(Addr paddr, ruby::MachineType machineType) const;

  /**
   * Helper function to check if a stream should
   * be migrated.
   */
  bool canMigrateStream(LLCDynStream *dynS) const;

  /**
   * Helper function to process stream data for indirect/update.
   */
  void triggerIndElems(LLCDynStreamPtr stream, LLCStreamElementPtr elem);
  void triggerIndElem(LLCDynStreamPtr IS, uint64_t indElemIdx);
  void triggerUpdate(LLCDynStreamPtr dynS, LLCStreamElementPtr elem,
                     const DynStreamSliceId &sliceId,
                     const ruby::DataBlock &storeValueBlock,
                     ruby::DataBlock &loadValueBlock, uint32_t &payloadSize);
  void triggerAtomic(LLCDynStreamPtr dynS, LLCStreamElementPtr elem,
                     const DynStreamSliceId &sliceId,
                     ruby::DataBlock *loadValueBlock, uint32_t &payloadSize);
  /**
   * Helper function to handle predicated-off element.
   */
  void predicateOffElem(LLCDynStreamPtr dynS, LLCStreamElementPtr elem);

  /**
   * API to manages LLCStreamSlices.
   * Slices are allocated from LLCDynStream and now managed by each
   * LLCStreamEngine.
   */
  using SliceList = std::list<LLCStreamSlicePtr>;
  LLCStreamSlicePtr allocateSlice(LLCDynStreamPtr dynS);
  LLCStreamSlicePtr tryGetSlice(const DynStreamSliceId &sliceId);
  SliceList::iterator tryGetSliceIter(const DynStreamSliceId &sliceId);
  SliceList::iterator releaseSlice(SliceList::iterator sliceIter);
  void processSlices();
  SliceList::iterator processSlice(SliceList::iterator sliceIter);
  void processLoadComputeSlice(LLCDynStreamPtr dynS, LLCStreamSlicePtr slice);
  void tryStartComputeLoadComputeSlice(LLCDynStreamPtr dynS,
                                       LLCStreamSlicePtr slice);
  void processDirectAtomicSlice(LLCDynStreamPtr dynS,
                                const DynStreamSliceId &sliceId);
  void processIndirectAtomicSlice(LLCDynStreamPtr dynS,
                                  const DynStreamSliceId &sliceId);
  void postProcessIndirectAtomicSlice(LLCDynStreamPtr dynS,
                                      const LLCStreamElementPtr &element);
  void processIndirectUpdateSlice(LLCDynStreamPtr dynS,
                                  const DynStreamSliceId &sliceId,
                                  const ruby::DataBlock &storeValueBlock);

  /**
   * Handle DirectUpdateSlice with the computation latency modelled.
   */
  bool tryProcessDirectUpdateSlice(LLCDynStreamPtr dynS,
                                   LLCStreamSlicePtr slice);
  bool tryPostProcessDirectUpdateSlice(LLCDynStreamPtr dynS,
                                       LLCStreamSlicePtr slice);
  void postProcessDirectUpdateSlice(LLCDynStreamPtr dynS,
                                    const DynStreamSliceId &sliceId);

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
  performStreamAtomicOp(LLCDynStreamPtr dynS, LLCStreamElementPtr element,
                        Addr elementPAddr, const DynStreamSliceId &sliceId);

  /**
   * Process the StreamForward request.
   */
  void processStreamFwdReq(const ruby::RequestMsg &req,
                           const DynStreamSliceId &recvSliceId);

  /**
   * Check if this is the second request to lock the indirect atomic, if so
   * process it.
   * @return whether this message is processed.
   */
  bool tryToProcessIndirectAtomicUnlockReq(const ruby::RequestMsg &req);

  /**
   * We handle the computation and charge its latency here.
   * The direct stream will also remember the number of incomplete computation
   * from itself (e.g. StoreStream with StoreFunc) and its indirect streams
   * (e.g. Reduction).
   */
  std::list<LLCStreamElementPtr> readyComputations;
  struct InflyComputation {
    LLCStreamElementPtr elem;
    StreamValue result;
    Cycles readyCycle;
    InflyComputation(const LLCStreamElementPtr &_elem,
                     const StreamValue &_result, Cycles _readyCycle)
        : elem(_elem), result(_result), readyCycle(_readyCycle) {}
  };
  int64_t numInflyRealCmps = 0;
  std::list<InflyComputation> inflyComputations;
  void tryVectorizeElem(LLCStreamElementPtr &elem, bool tryVectorize);
  void pushReadyComputation(LLCStreamElementPtr &elem,
                            bool tryVectorize = false);
  void skipComputation(LLCStreamElementPtr &elem);
  void pushInflyComputation(LLCStreamElementPtr &elem,
                            const StreamValue &result, Cycles &latency);
  void recordComputationMicroOps(Stream *S);
  void startComputation();
  void completeComputation();

  void incrementIssueSlice(StreamStatistic &statistic);

  void sampleLLCStreams();
  static Cycles lastSampleCycle;
  static int totalSamples;

  StreamFloatTracer seTracer;
  void traceEvent(Cycles cycle,
                  ::LLVM::TDG::StreamFloatEvent::StreamFloatEventType event);
  void traceEvent(::LLVM::TDG::StreamFloatEvent::StreamFloatEventType event);
};

} // namespace gem5

#endif