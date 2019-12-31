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
  MLCDynamicDirectStream(CacheStreamConfigureData *_configData,
                         AbstractStreamAwareController *_controller,
                         MessageBuffer *_responseMsgBuffer,
                         MessageBuffer *_requestToLLCMsgBuffer);

  /**
   * Get where is the LLC stream is at the end of current allocated credits.
   */
  Addr getLLCStreamTailPAddr() const override { return this->llcTailPAddr; }

  /**
   * Set the indirect stream.
   */
  void addIndirectStream(MLCDynamicIndirectStream *indirectStream) {
    assert(!this->indirectStream && "More than one indirect stream.");
    this->indirectStream = indirectStream;
  }

  void receiveStreamData(const ResponseMsg &msg) override;

protected:
  SlicedDynamicStream slicedStream;

  // Where the LLC stream would be at tailSliceIdx.
  Addr tailPAddr;
  MachineID tailSliceLLCBank;

  // Where the LLC stream's tail index is.
  uint64_t llcTailSliceIdx;
  // Where the LLC stream currently would be, given the credit limit.
  Addr llcTailPAddr;
  MachineID llcTailSliceLLCBank;

  // Only support one indirect stream.
  MLCDynamicIndirectStream *indirectStream;

  bool hasOverflowed() const override {
    return this->slicedStream.hasOverflowed();
  }
  int64_t getTotalTripCount() const override {
    return this->slicedStream.getTotalTripCount();
  }

  /**
   * Override this as we need to send credit to llc.
   */
  void advanceStream() override;
  void allocateSlice();

  /**
   * Send credit to the LLC stream. Update the llcTailSliceIdx.
   */
  void sendCreditToLLC();

  /**
   * Notify the indirect stream that I have data.
   */
  void notifyIndirectStream(const MLCStreamSlice &slice);
};

#endif