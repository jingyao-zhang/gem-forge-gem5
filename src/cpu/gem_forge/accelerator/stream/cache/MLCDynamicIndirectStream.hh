#ifndef __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_INDIRECT_STREAM_H__
#define __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_INDIRECT_STREAM_H__

#include "MLCDynamicStream.hh"

/**
 * MLCDynamicIndirectStream is a special stream.
 * 1. It does not send credit to LLC. The direct stream should perform the flow
 * control for the indirect stream.
 * 2. It always allocates elements one by one. No merge elements even if they
 * are from the same cache line.
 */
class MLCDynamicIndirectStream : public MLCDynamicStream {
public:
  MLCDynamicIndirectStream(CacheStreamConfigureData *_configData,
                           AbstractStreamAwareController *_controller,
                           MessageBuffer *_responseMsgBuffer,
                           MessageBuffer *_requestToLLCMsgBuffer,
                           const DynamicStreamId &_rootStreamId);

  virtual ~MLCDynamicIndirectStream() {}

  const DynamicStreamId &getRootDynamicStreamId() const override {
    return this->rootStreamId;
  }

  bool isSliceValid(const DynamicStreamSliceId &sliceId) const override {
    assert(sliceId.getNumElements() == 1 &&
           "Multiple elements for indirect stream.");
    if (this->isOneIterationBehind) {
      if (sliceId.lhsElementIdx == 0) {
        return false;
      }
    }
    return true;
  }

  void receiveStreamData(const ResponseMsg &msg) override;

private:
  // Remember the root stream id.
  DynamicStreamId rootStreamId;
  // Remember if this indirect stream is behind one iteration.
  bool isOneIterationBehind;

  /**
   * -1 means indefinite.
   */
  const int64_t totalTripCount;

  bool hasOverflowed() const override;
  int64_t getTotalTripCount() const override;
  bool matchSliceId(const DynamicStreamSliceId &A,
                    const DynamicStreamSliceId &B) const override {
    // Indirect stream can just match the lhsElementIdx.
    return A.lhsElementIdx == B.lhsElementIdx;
  }

  void advanceStream() override;
  void allocateSlice();
};

#endif