#ifndef __CPU_TDG_ACCELERATOR_LLC_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_LLC_STREAM_ENGINE_H__

#include "LLCDynamicStream.hh"

// Generate by slicc.
#include "mem/ruby/protocol/RequestMsg.hh"

#include "mem/ruby/common/Consumer.hh"

/**
 * Derive from Consumer to schedule wakeup event.
 */
#include <list>
#include <memory>

class AbstractStreamAwareController;
class MessageBuffer;

class LLCStreamEngine : public Consumer {
public:
  LLCStreamEngine(AbstractStreamAwareController *_controller,
                  MessageBuffer *_streamMigrateMsgBuffer,
                  MessageBuffer *_streamIssueMsgBuffer,
                  MessageBuffer *_streamIndirectIssueMsgBuffer);
  ~LLCStreamEngine();

  void receiveStreamConfigure(PacketPtr pkt);
  void receiveStreamEnd(PacketPtr pkt);
  void receiveStreamMigrate(LLCDynamicStreamPtr stream);
  void receiveStreamFlow(const DynamicStreamSliceId &sliceId);
  void receiveStreamElementData(const DynamicStreamSliceId &sliceId,
                                const DataBlock &dataBlock);
  void receiveStreamIndirectRequest(const RequestMsg &req);
  void wakeup() override;
  void print(std::ostream &out) const override;

private:
  AbstractStreamAwareController *controller;
  // Out going stream migrate buffer.
  MessageBuffer *streamMigrateMsgBuffer;
  MessageBuffer *streamIssueMsgBuffer;
  MessageBuffer *streamIndirectIssueMsgBuffer;
  const int issueWidth;
  const int migrateWidth;
  // Threshold to limit maximum number of infly requests.
  const int maxInflyRequests;
  // Threshold to limit maximum number of requests in queue;
  const int maxInqueueRequests;

  using StreamList = std::list<LLCDynamicStream *>;
  using StreamListIter = StreamList::iterator;
  StreamList streams;
  /**
   * Streams waiting to be migrated to other LLC bank.
   */
  StreamList migratingStreams;

  /**
   * Buffered stream flow message waiting for the stream
   * to migrate here.
   */
  std::list<DynamicStreamSliceId> pendingStreamFlowControlMsgs;

  /**
   * Buffered stream end message waiting for the stream
   * to migrate here.
   */
  std::unordered_set<DynamicStreamId, DynamicStreamIdHasher>
      pendingStreamEndMsgs;

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
   * Issue a single stream.
   */
  bool issueStream(LLCDynamicStream *stream);

  /**
   * Issue the indirect elements for a stream.
   */
  bool issueStreamIndirect(LLCDynamicStream *stream);

  /**
   * Helper function to issue stream request to the LLC cache bank.
   */
  void issueStreamRequestToLLCBank(
      LLCDynamicStream *stream, Addr paddrLine,
      const DynamicStreamSliceId &sliceId,
      CoherenceRequestType requestType = CoherenceRequestType_GETU,
      uint64_t storeData = 0);

  /**
   * Migrate streams.
   */
  void migrateStreams();

  /**
   * Migrate a single stream.
   */
  void migrateStream(LLCDynamicStream *stream);

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
                                           const DynamicStreamSliceId &sliceId,
                                           const DataBlock &dataBlock);
  void processStreamDataForUpdateStream(LLCDynamicStreamPtr stream,
                                        const DynamicStreamSliceId &sliceId,
                                        const DataBlock &dataBlock);
  uint64_t extractElementDataFromSlice(LLCDynamicStreamPtr stream,
                                       uint64_t elementIdx,
                                       const DataBlock &dataBlock);
  void updateElementData(LLCDynamicStreamPtr stream, uint64_t elementIdx,
                         uint64_t updateValue);
};

#endif