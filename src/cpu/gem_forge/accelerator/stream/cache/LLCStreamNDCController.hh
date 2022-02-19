#ifndef __CPU_GEM_FORGE_LLC_STREAM_NDC_CONTROLLER_HH__
#define __CPU_GEM_FORGE_LLC_STREAM_NDC_CONTROLLER_HH__

#include "LLCStreamEngine.hh"

#include "cpu/gem_forge/accelerator/stream/stream_ndc_packet.hh"

class LLCStreamNDCController {
public:
  LLCStreamNDCController(LLCStreamEngine *_llcSE);

  void receiveStreamNDCRequest(PacketPtr pkt);

  void receiveStreamData(const DynStreamSliceId &sliceId,
                         const DataBlock &dataBlock,
                         const DataBlock &storeValueBlock);
  void receiveStreamForwardRequest(const RequestMsg &msg);

  /**
   * Compute the element value.
   * @return whether we created valid result.
   */
  bool computeStreamElementValue(const LLCStreamElementPtr &element,
                                 StreamValue &result);

  /**
   * Complete the NDC computation.
   */
  void completeComputation(const LLCStreamElementPtr &element,
                           const StreamValue &value);

  static void allocateContext(AbstractStreamAwareController *mlcController,
                              StreamNDCPacketPtr &streamNDC);

private:
  LLCStreamEngine *llcSE;

  struct NDCContext {
    StreamNDCPacketPtr ndc;
    LLCStreamElementPtr element;
    NDCContext(const StreamNDCPacketPtr &_ndc, LLCStreamElementPtr &_element)
        : ndc(_ndc), element(_element) {}
    /**
     * More runtime states.
     */
    bool cacheLineReady = false;
    int receivedForward = 0;
  };

  using NDCContextMapT =
      std::unordered_map<DynStreamId, std::list<NDCContext>, DynStreamIdHasher>;
  static NDCContextMapT inflyNDCContextMap;

  static NDCContext *getContextFromSliceId(const DynStreamSliceId &sliceId);
  static void eraseContextFromSliceId(const DynStreamSliceId &sliceId);

  void processStreamNDCRequest(PacketPtr pkt);

  /**
   * Helper function to issue stream NDC response to MLC at request core.
   */
  void issueStreamNDCResponseToMLC(const DynStreamSliceId &sliceId,
                                   Addr paddrLine, const uint8_t *data,
                                   int dataSize, int payloadSize,
                                   int lineOffset, bool forceIdea);

  void handleNDC(NDCContext &context, const DynStreamSliceId &sliceId,
                 const DataBlock &dataBlock);
  void handleAtomicNDC(NDCContext &context,
                       const DynStreamSliceId &sliceId);
  void handleForwardNDC(NDCContext &context,
                        const DynStreamSliceId &sliceId,
                        const DataBlock &dataBlock);
  void handleStoreNDC(NDCContext &context);
};

#endif
