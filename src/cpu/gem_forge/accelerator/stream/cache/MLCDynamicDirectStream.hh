#ifndef __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_DIRECT_STREAM_H__
#define __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_DIRECT_STREAM_H__

#include "DynamicStreamSliceIdVec.hh"
#include "MLCDynamicStream.hh"
#include "SlicedDynamicStream.hh"

class MLCDynamicIndirectStream;

/**
 * Direct MLCStream should handle flow control.
 * Also will slice the stream into cache lines.
 */
class MLCDynamicDirectStream : public MLCDynamicStream {
public:
  MLCDynamicDirectStream(
      CacheStreamConfigureDataPtr _configData,
      AbstractStreamAwareController *_controller,
      MessageBuffer *_responseMsgBuffer, MessageBuffer *_requestToLLCMsgBuffer,
      const std::vector<MLCDynamicIndirectStream *> &_indirectStreams);

  /**
   * Get where is the RemoteStream is at the end of current allocated credits.
   */
  std::pair<Addr, MachineType>
  getRemoteTailPAddrAndMachineType() const override;

  void receiveStreamData(const DynamicStreamSliceId &sliceId,
                         const DataBlock &dataBlock, Addr paddrLine) override;
  void receiveReuseStreamData(Addr vaddr, const DataBlock &dataBlock);
  void setLLCCutLineVAddr(Addr vaddr) { this->llcCutLineVAddr = vaddr; }

  void receiveStreamDone(const DynamicStreamSliceId &sliceId) override;

  /**
   * Check the core's commit progress and send out StreamCommit message to
   * LLC banks.
   */
  void checkCoreCommitProgress();

  /**
   * We query the SlicedStream for TotalTripCount.
   */
  bool hasOverflowed() const override {
    return this->slicedStream.hasOverflowed();
  }
  int64_t getTotalTripCount() const override {
    return this->slicedStream.getTotalTripCount();
  }
  bool hasTotalTripCount() const override {
    return this->slicedStream.hasTotalTripCount();
  }
  void setTotalTripCount(int64_t totalTripCount, Addr brokenPAddr) override;

protected:
  SlicedDynamicStream slicedStream;

  uint64_t maxNumSlicesPerSegment;

  /**
   * For reuse pattern, store the cut information.
   */
  Addr llcCutLineVAddr = 0;
  uint64_t llcCutSliceIdx = 0;
  bool llcCutted = false;

  // Where the LLC stream would be at tailSliceIdx.
  DynamicStreamSliceIdVec nextSegmentSliceIds;
  Addr tailPAddr;
  DynamicStreamSliceId tailSliceId;

  // This stream has been cut by LLCStreamBound.
  bool llcStreamLoopBoundCutted = false;
  Addr llcStreamLoopBoundBrokenPAddr = 0;
  MachineType llcStreamLoopBoundBrokenMachineType = MachineType_NULL;

  struct LLCSegmentPosition {
    /**
     * Remember the start and end position in LLC banks.
     */
    Addr startPAddr = 0;
    Addr endPAddr = 0;
    uint64_t startSliceIdx = 0;
    uint64_t endSliceIdx = 0;
    DynamicStreamSliceIdVec sliceIds;
    DynamicStreamSliceId endSliceId;
    enum State {
      ALLOCATED = 0,
      CREDIT_SENT,
      COMMITTING,
      COMMITTED,
    };
    State state = State::ALLOCATED;
    static std::string stateToString(const State state);
    const DynamicStreamSliceId &getStartSliceId() const {
      return this->sliceIds.firstSliceId();
    }
  };

  /**
   * Split the segments into multiple lists to improve the performance.
   */
  std::list<LLCSegmentPosition> llcSegmentsAllocated;
  std::list<LLCSegmentPosition> llcSegments;

  bool blockedOnReceiverElementInit = false;

  void allocateLLCSegment();
  void pushNewLLCSegment(Addr startPAddr, uint64_t startSliceIdx);
  LLCSegmentPosition &getLastLLCSegment();
  const LLCSegmentPosition &getLastLLCSegment() const;
  uint64_t getLLCTailSliceIdx() const {
    return this->getLastLLCSegment().endSliceIdx;
  }

  std::unordered_map<Addr, DataBlock> reuseBlockMap;

  std::vector<MLCDynamicIndirectStream *> indirectStreams;

  bool matchLLCSliceId(const DynamicStreamSliceId &mlc,
                       const DynamicStreamSliceId &llc) const {
    if (this->config->isPointerChase) {
      return mlc.getStartIdx() == llc.getStartIdx() && mlc.vaddr == llc.vaddr;
    } else {
      // By default match the vaddr.
      // TODO: This is really wrong.
      return mlc.vaddr == llc.vaddr;
    }
  }

  bool matchCoreSliceId(const DynamicStreamSliceId &mlc,
                        const DynamicStreamSliceId &core) const {
    /**
     * Core request always has BlockVAddr.
     */
    if (this->config->isPointerChase) {
      return mlc.getStartIdx() == core.getStartIdx() &&
             makeLineAddress(mlc.vaddr) == makeLineAddress(core.vaddr);
    } else {
      // By default match the vaddr.
      // TODO: This is really wrong.
      return mlc.vaddr == core.vaddr;
    }
  }

  SliceIter
  findSliceForCoreRequest(const DynamicStreamSliceId &sliceId) override;

  /**
   * Override this as we need to send credit to llc.
   */
  void advanceStream() override;
  void allocateSlice();

  /**
   * Check and send credit to the LLC stream. Enqueue a new segment.
   */
  void trySendCreditToLLC();
  void sendCreditToLLC(const LLCSegmentPosition &segment);

  /**
   * Send commit message to the LLC stream.
   */
  void sendCommitToLLC(const LLCSegmentPosition &segment);

  /**
   * Notify the indirect stream that I have data.
   */
  void notifyIndirectStream(const MLCStreamSlice &slice);
};

#endif