#ifndef __CPU_GEM_FORGE_MLC_DYN_STREAM_H__
#define __CPU_GEM_FORGE_MLC_DYN_STREAM_H__

#include "DynStreamAddressRange.hh"

#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include "mem/ruby/common/DataBlock.hh"

// Generated by slicc.
#include "mem/ruby/protocol/ResponseMsg.hh"

#include <list>

class AbstractStreamAwareController;
class MessageBuffer;

class MLCDynStream {
public:
  MLCDynStream(CacheStreamConfigureDataPtr _configData,
               AbstractStreamAwareController *_controller,
               MessageBuffer *_responseMsgBuffer,
               MessageBuffer *_requestToLLCMsgBuffer, bool _isMLCDirect);

  virtual ~MLCDynStream();

  Stream *getStaticStream() const { return this->stream; }

  const DynStrandId &getDynStrandId() const { return this->strandId; }
  const DynStreamId &getDynStreamId() const {
    return this->strandId.dynStreamId;
  }
  DynStream *getCoreDynS() const {
    return this->getStaticStream()->getDynStream(this->getDynStreamId());
  }

  bool getIsPseudoOffload() const { return this->isPseudoOffload; }
  uint64_t getFirstFloatElemIdx() const {
    return this->config->floatPlan.getFirstFloatElementIdx();
  }

  virtual const DynStreamId &getRootDynStreamId() const {
    // By default this we are the root stream.
    return this->getDynStreamId();
  }

  /**
   * Helper function to check if a slice is valid within this stream context.
   * So far always valid, except the first element of indirect stream that is
   * behind by one iteration.
   */
  virtual bool isSliceValid(const DynStreamSliceId &sliceId) const {
    return true;
  }

  /**
   * Get where is the RemoteStream is at the end of current allocated credits.
   */
  virtual std::pair<Addr, MachineType>
  getRemoteTailPAddrAndMachineType() const {
    panic("Should only call this on direct stream.");
  }

  virtual void receiveStreamData(const DynStreamSliceId &sliceId,
                                 const DataBlock &dataBlock,
                                 Addr paddrLine) = 0;
  void receiveStreamRequest(const DynStreamSliceId &sliceId);
  void receiveStreamRequestHit(const DynStreamSliceId &sliceId);

  /**
   * Before end the stream, we have make dummy response to the request
   * we have seen to make the ruby system happy.
   */
  void endStream();

  uint64_t getHeadSliceIdx() const { return this->headSliceIdx; }
  uint64_t getTailSliceIdx() const { return this->tailSliceIdx; }

  void receiveStreamRange(const DynStreamAddressRangePtr &range);
  virtual void receiveStreamDone(const DynStreamSliceId &sliceId);

  void scheduleAdvanceStream();

  /**
   * Whether this stream requires range-based syncrhonization.
   */
  bool shouldRangeSync() const { return this->config->rangeSync; }

  const std::vector<CacheStreamConfigureData::DepEdge> &getSendToEdges() const {
    return this->sendToEdges;
  }

  /**
   * API for this to check if overflowed.
   */
  virtual bool hasOverflowed() const = 0;
  virtual int64_t getTotalTripCount() const = 0;
  virtual bool hasTotalTripCount() const = 0;
  virtual int64_t getInnerTripCount() const = 0;
  virtual bool hasInnerTripCount() const = 0;
  virtual void setTotalTripCount(int64_t totalTripCount, Addr brokenPAddr,
                                 MachineType brokenMachineType) = 0;

  bool isWaitingAck() const { return this->isWaiting == WaitType::Ack; }
  bool isWaitingData() const { return this->isWaiting == WaitType::Data; }
  bool isWaitingNothing() const { return this->isWaiting == WaitType::Nothing; }

protected:
  Stream *stream;
  DynStrandId strandId;
  CacheStreamConfigureDataPtr config;
  bool isPointerChase;
  bool isPseudoOffload;
  const bool isMLCDirect;

  std::vector<CacheStreamConfigureData::DepEdge> sendToEdges;

  AbstractStreamAwareController *controller;
  MessageBuffer *responseMsgBuffer;
  MessageBuffer *requestToLLCMsgBuffer;
  uint64_t maxNumSlices;

  /**
   * Represent an allocated stream slice at MLC.
   * Used as a meeting point for the request from core
   * and data from LLC stream engine.
   */
  struct MLCStreamSlice {
    DynStreamSliceId sliceId;
    DataBlock dataBlock;
    // Whether the core's request is already here.
    bool dataReady;
    enum CoreStatusE {
      NONE,
      WAIT_DATA, // The core is waiting the data.
      WAIT_ACK,  // The core is waiting the ack.
      ACK_READY, // The ack is ready, waiting to be reported to core in order.
      DONE,
      FAULTED
    };
    CoreStatusE coreStatus;
    // For debug purpose, we also remember core's request sliceId.
    DynStreamSliceId coreSliceId;
    // Statistics.
    Cycles dataReadyCycle;
    Cycles coreWaitCycle;

    MLCStreamSlice(const DynStreamSliceId &_sliceId)
        : sliceId(_sliceId), dataBlock(), dataReady(false),
          coreStatus(CoreStatusE::NONE) {}

    void setData(const DataBlock &dataBlock, Cycles currentCycle) {
      assert(!this->dataReady && "Data already ready.");
      this->dataBlock = dataBlock;
      this->dataReady = true;
      this->dataReadyCycle = currentCycle;
    }

    static std::string convertCoreStatusToString(CoreStatusE status);
  };

  std::list<MLCStreamSlice> slices;
  using SliceIter = std::list<MLCStreamSlice>::iterator;
  // Slice index of allocated [head, tail).
  uint64_t headSliceIdx;
  uint64_t tailSliceIdx;

  EventFunctionWrapper advanceStreamEvent;

  virtual void advanceStream() = 0;
  void makeResponse(MLCStreamSlice &slice);
  void makeAck(MLCStreamSlice &slice);

  /**
   * Find the correct slice for a core request.
   * Used in receiveStreamRequest() and receiveStreamRequestHit().
   */
  virtual SliceIter
  findSliceForCoreRequest(const DynStreamSliceId &sliceId) = 0;

  /**
   * Helper function to translate the vaddr to paddr.
   */
  Addr translateVAddr(Addr vaddr) const;

  /**
   * Helper function to direct read data from memory.
   */
  void readBlob(Addr vaddr, uint8_t *data, int size) const;

  /**
   * Pop slices. Flags to remember why we are blocked.
   * @return whether popped at least one slice.
   */
  bool tryPopStream();
  void popOneSlice();
  bool popBlocked = false;

  /**
   * These function checks if we are waiting for something.
   */
  enum WaitType {
    Nothing,
    Ack,
    Data,
  };
  std::string to_string(WaitType type) {
    switch (type) {
    case WaitType::Nothing:
      return "Nothing";
    case WaitType::Ack:
      return "Ack";
    case WaitType::Data:
      return "Data";
    default:
      return "Unknown";
    }
  }
  WaitType isWaiting;
  WaitType checkWaiting() const;

  bool isCoreDynSReleased() const { return !this->getCoreDynS(); }

  /**
   * This remember the received StreamRange.
   */
  std::list<DynStreamAddressRangePtr> receivedRanges;

public:
  /**
   * A helper function to dump some basic status of the stream when panic.
   */
  void panicDump() const;
};

#endif
