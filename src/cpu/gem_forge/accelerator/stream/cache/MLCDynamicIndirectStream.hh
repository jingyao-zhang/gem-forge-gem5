#ifndef __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_INDIRECT_STREAM_H__
#define __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_INDIRECT_STREAM_H__

#include "MLCDynamicStream.hh"

/**
 * MLCDynamicIndirectStream is a special stream.
 * 1. It does not send credit to LLC. The direct stream should perform the flow
 * control for the indirect stream.
 * 2. It always allocates elements one by one. No merge elements even if they
 * are from the same cache line, i.e. one slice must belong to one single
 * element.
 * 3. Due to coalescing, an indirect stream element may span multiple cache
 * lines. So we need to match lhsElementIdx and vaddr to find the correct slice.
 *
 * Indirect streams are more complicated to manage, as the address is only know
 * when the base stream data is ready.
 */
class MLCDynamicIndirectStream : public MLCDynamicStream {
public:
  MLCDynamicIndirectStream(CacheStreamConfigureDataPtr _configData,
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
      if (sliceId.getStartIdx() == 0) {
        return false;
      }
    }
    return true;
  }

  /**
   * Receive data from LLC.
   */
  void receiveStreamData(const DynamicStreamSliceId &sliceId,
                         const DataBlock &dataBlock, Addr paddrLine) override;

  /**
   * Receive data from the base direct stream.
   * Try to generate more slices.
   */
  void receiveBaseStreamData(uint64_t elementIdx, uint64_t baseData);

  void setBaseStream(MLCDynamicStream *baseStream) {
    assert(!this->baseStream && "Already has base stream.");
    this->baseStream = baseStream;
  }

private:
  // Remember the root stream id.
  DynamicStreamId rootStreamId;
  DynamicStreamFormalParamV formalParams;
  AddrGenCallbackPtr addrGenCallback;
  const int32_t elementSize;
  MLCDynamicStream *baseStream = nullptr;

  // Remember if this indirect stream is behind one iteration.
  bool isOneIterationBehind;

  /**
   * -1 means indefinite.
   */
  const int64_t totalTripCount;

  /**
   * The tail ElementIdx (Not allocated yet).
   * This is not the same as tailSliceIdx due to coalesced indirect stream:
   * a[b[i] + 0]; a[b[i] + 1];
   */
  uint64_t tailElementIdx;

  bool hasOverflowed() const override;
  int64_t getTotalTripCount() const override;
  SliceIter
  findSliceForCoreRequest(const DynamicStreamSliceId &sliceId) override;

  void advanceStream() override;
  void allocateSlice();
  Addr genElementVAddr(uint64_t elementIdx, uint64_t baseData);

  /**
   * Find the begin and end of the substream that belongs to an elementIdx.
   */
  std::pair<SliceIter, SliceIter> findSliceByElementIdx(uint64_t elementIdx);

  /**
   * Try to find the slice with the given vaddr. If failed, insert one and
   * maintain the sorted order.
   */
  SliceIter findOrInsertSliceBySliceId(const SliceIter &begin,
                                       const SliceIter &end,
                                       const DynamicStreamSliceId &sliceId);
};

#endif