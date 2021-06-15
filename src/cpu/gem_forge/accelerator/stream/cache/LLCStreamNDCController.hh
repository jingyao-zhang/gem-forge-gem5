#ifndef __CPU_GEM_FORGE_LLC_STREAM_NDC_CONTROLLER_HH__
#define __CPU_GEM_FORGE_LLC_STREAM_NDC_CONTROLLER_HH__

#include "LLCStreamEngine.hh"

#include "cpu/gem_forge/accelerator/stream/stream_ndc_packet.hh"

class LLCStreamNDCController {
public:
  LLCStreamNDCController(LLCStreamEngine *_llcSE);

  void receiveStreamNDCRequest(PacketPtr pkt);

  void receiveStreamData(const DynamicStreamSliceId &sliceId,
                         const DataBlock &dataBlock,
                         const DataBlock &storeValueBlock);

private:
  LLCStreamEngine *llcSE;

  struct NDCContext {
    StreamNDCPacketPtr ndc;
    NDCContext(const StreamNDCPacketPtr &_ndc) : ndc(_ndc) {}
  };

  std::unordered_map<DynamicStreamId, std::list<NDCContext>,
                     DynamicStreamIdHasher>
      inflyNDCContextMap;

  NDCContext &getContextFromSliceId(const DynamicStreamSliceId &sliceId);
  void eraseContextFromSliceId(const DynamicStreamSliceId &sliceId);

  void processStreamNDCRequest(PacketPtr pkt);

  /**
   * Helper function to issue stream NDC response to MLC at request core.
   */
  void issueStreamNDCResponseToMLC(const DynamicStreamSliceId &sliceId,
                                   Addr paddrLine, const uint8_t *data,
                                   int dataSize, int payloadSize,
                                   int lineOffset, bool forceIdea);
};

#endif
