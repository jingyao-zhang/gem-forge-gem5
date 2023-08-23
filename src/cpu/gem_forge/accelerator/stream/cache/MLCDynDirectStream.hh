#ifndef __CPU_GEM_FORGE_MLC_DYN_DIRECT_STREAM_H__
#define __CPU_GEM_FORGE_MLC_DYN_DIRECT_STREAM_H__

#include "DynStreamSliceIdVec.hh"
#include "MLCDynStream.hh"
#include "SlicedDynStream.hh"

namespace gem5 {

class MLCDynIndirectStream;

/**
 * Direct MLCStream should handle flow control.
 * Also will slice the stream into cache lines.
 */
class MLCDynDirectStream : public MLCDynStream {
public:
  MLCDynDirectStream(
      CacheStreamConfigureDataPtr _configData,
      ruby::AbstractStreamAwareController *_controller,
      ruby::MessageBuffer *_responseMsgBuffer,
      ruby::MessageBuffer *_requestToLLCMsgBuffer,
      const std::vector<MLCDynIndirectStream *> &_indirectStreams);

  /**
   * Get where is the RemoteStream is at the end of current allocated credits.
   */
  std::pair<Addr, ruby::MachineType>
  getRemoteTailPAddrAndMachineType() const override;

  void receiveStreamData(const DynStreamSliceId &sliceId,
                         const ruby::DataBlock &dataBlock, Addr paddrLine,
                         bool isAck) override;
  void receiveReuseStreamData(Addr vaddr, const ruby::DataBlock &dataBlock);
  void setLLCCutLineVAddr(Addr vaddr) { this->llcCutLineVAddr = vaddr; }

  void receiveStreamDone(const DynStreamSliceId &sliceId) override;

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
  int64_t getInnerTripCount() const override {
    return this->slicedStream.getInnerTripCount();
  }
  bool hasInnerTripCount() const override {
    return this->slicedStream.hasInnerTripCount();
  }
  void breakOutLoop(int64_t totalTripCount) override;

  void sample() const override;

  uint64_t getNextCreditElemIdx() const override {
    return this->nextCreditElemIdx;
  }

protected:
  SlicedDynStream slicedStream;

  uint64_t maxNumSlicesPerSegment;
  uint64_t maxNumRunaheadSlices;

  /**
   * For reuse pattern, store the cut information.
   */
  Addr llcCutLineVAddr = 0;
  uint64_t llcCutSliceIdx = 0;
  bool llcCutted = false;

  // Where the LLC stream would be at tailSliceIdx.
  DynStreamSliceIdVec nextSegmentSliceIds;
  Addr tailPAddr;
  DynStreamSliceId tailSliceId;

  struct LLCSegmentPosition {
    /**
     * Remember the start and end position in LLC banks.
     */
    Addr startPAddr = 0;
    Addr endPAddr = 0;
    uint64_t startSliceIdx = 0;
    uint64_t endSliceIdx = 0;
    DynStreamSliceIdVec sliceIds;
    DynStreamSliceId endSliceId;
    enum State {
      ALLOCATED = 0,
      CREDIT_SENT,
      COMMITTING,
      COMMITTED,
    };
    State state = State::ALLOCATED;
    static std::string stateToString(const State state);
    const DynStreamSliceId &getStartSliceId() const {
      return this->sliceIds.firstSliceId();
    }
  };

  /**
   * Split the segments into multiple lists to improve the performance.
   */
  using LLCSegmentPositionListT = std::list<LLCSegmentPosition>;
  LLCSegmentPositionListT llcSegmentsAllocated;
  LLCSegmentPositionListT llcSegments;

  bool blockedSendingCredit = false;

  void allocateLLCSegment();
  void pushNewLLCSegment(Addr startPAddr, uint64_t startSliceIdx);
  LLCSegmentPosition &getLastLLCSegment();
  const LLCSegmentPosition &getLastLLCSegment() const;
  bool hasLastCreditedLLCSegment() const;
  const LLCSegmentPosition &getLastCreditedLLCSegment() const;
  uint64_t getLLCTailSliceIdx() const {
    return this->getLastLLCSegment().endSliceIdx;
  }

  std::unordered_map<Addr, ruby::DataBlock> reuseBlockMap;

  std::vector<MLCDynIndirectStream *> indirectStreams;

  bool matchLLCSliceId(const DynStreamSliceId &mlc,
                       const DynStreamSliceId &llc) const {
    return mlc.getStartIdx() == llc.getStartIdx() && mlc.vaddr == llc.vaddr;
  }

  bool matchCoreSliceId(const DynStreamSliceId &mlc,
                        const DynStreamSliceId &core) const {
    /**
     * Core request always has BlockVAddr.
     */
    if (this->config->isPointerChase) {
      return mlc.getStartIdx() == core.getStartIdx() &&
             ruby::makeLineAddress(mlc.vaddr) ==
                 ruby::makeLineAddress(core.vaddr);
    } else {
      // By default match the vaddr.
      // TODO: This is really wrong.
      return mlc.vaddr == core.vaddr;
    }
  }

  SliceIter findSliceForCoreRequest(const DynStreamSliceId &sliceId) override;

  /**
   * Override this as we need to send credit to llc.
   */
  void advanceStream() override;
  void allocateSlice();

  /**
   * Try to release LLC segments if we do not need range-sync.
   */
  void tryReleaseNonRangeSyncSegment();

  /**
   * Check and send credit to the LLC stream. Enqueue a new segment.
   */
  void trySendCreditToLLC();
  void sendCreditToLLC(const LLCSegmentPosition &segment);

  /**
   * Check whether we are running ahead than the LLCDynS.
   */
  bool checkWaitForLLCRecvS(uint64_t tailStrandElemIdx,
                            DynStrandId &waitForRecvStrandId,
                            uint64_t &waitForRecvStrandElemIdx) const;

  /**
   * Send commit message to the LLC stream.
   */
  void sendCommitToLLC(const LLCSegmentPosition &segment);

  /**
   * Notify the indirect stream that I have data.
   */
  void notifyIndStreams(const MLCStreamSlice &slice);

  bool isInConstructor = false;

  /**
   * Remeber the last paddr to send credit to. Initialized to config->initPAddr.
   * Used to implement NonMigrating stream.
   */
  Addr lastCreditPAddr = 0;

  /**
   * Remember the next credit element idx.
   */
  uint64_t nextCreditElemIdx = 0;

  /**
   * Always remember the last segement.
   */
  bool lastSegmentValid = false;
  LLCSegmentPosition lastSegment;
};

} // namespace gem5

#endif