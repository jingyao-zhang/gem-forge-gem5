#ifndef __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_DIRECT_STREAM_H__
#define __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_DIRECT_STREAM_H__

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
   * Get where is the LLC stream is at the end of current allocated credits.
   */
  Addr getLLCTailPAddr() const override {
    return this->getLastLLCSegment().endPAddr;
  }

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

protected:
  SlicedDynamicStream slicedStream;

  /**
   * For reuse pattern, store the cut information.
   */
  Addr llcCutLineVAddr = 0;
  uint64_t llcCutSliceIdx = 0;
  bool llcCutted = false;

  // Where the LLC stream would be at tailSliceIdx.
  Addr tailPAddr;
  DynamicStreamSliceId tailSliceId;

  struct LLCSegmentPosition {
    /**
     * Remember the start and end position in LLC banks.
     */
    Addr startPAddr = 0;
    Addr endPAddr = 0;
    uint64_t startSliceIdx = 0;
    uint64_t endSliceIdx = 0;
    DynamicStreamSliceId startSliceId;
    DynamicStreamSliceId endSliceId;
    enum State {
      ALLOCATED = 0,
      COMMITTING,
      COMMITTED,
    };
    State state = State::ALLOCATED;
    static std::string stateToString(const State state);
  };
  std::list<LLCSegmentPosition> llcSegments;

  void pushNewLLCSegment(Addr startPAddr, uint64_t startSliceIdx,
                         const DynamicStreamSliceId &startSliceId);
  LLCSegmentPosition &getLastLLCSegment();
  const LLCSegmentPosition &getLastLLCSegment() const;
  uint64_t getLLCTailSliceIdx() const {
    return this->getLastLLCSegment().endSliceIdx;
  }

  std::unordered_map<Addr, DataBlock> reuseBlockMap;

  std::vector<MLCDynamicIndirectStream *> indirectStreams;

  bool hasOverflowed() const override {
    return this->slicedStream.hasOverflowed();
  }
  int64_t getTotalTripCount() const override {
    return this->slicedStream.getTotalTripCount();
  }

  SliceIter
  findSliceForCoreRequest(const DynamicStreamSliceId &sliceId) override;

  /**
   * Override this as we need to send credit to llc.
   */
  void advanceStream() override;
  void allocateSlice();

  /**
   * Send credit to the LLC stream. Enqueue a new segment.
   */
  void sendCreditToLLC();

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