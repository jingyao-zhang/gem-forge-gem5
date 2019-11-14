#include "MLCDynamicIndirectStream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/RubyStream.hh"

#define DEBUG_TYPE RubyStream
#include "../stream_log.hh"

MLCDynamicIndirectStream::MLCDynamicIndirectStream(
    CacheStreamConfigureData *_configData,
    AbstractStreamAwareController *_controller,
    MessageBuffer *_responseMsgBuffer, MessageBuffer *_requestToLLCMsgBuffer,
    const DynamicStreamId &_rootStreamId)
    : MLCDynamicStream(_configData, _controller, _responseMsgBuffer,
                       _requestToLLCMsgBuffer, false /*mergeElements*/),
      rootStreamId(_rootStreamId),
      isOneIterationBehind(_configData->isOneIterationBehind) {
  if (this->isOneIterationBehind) {
    // This indirect stream is behind one iteration, which means that the first
    // element is not handled by LLC stream. The stream buffer should start at
    // the second element. We simply release the first element here.
    assert(!this->slices.empty() && "No initial slices.");
    // Let's do some sanity check.
    auto &firstSliceId = this->slices.front().sliceId;
    assert(firstSliceId.startIdx == 0 && "Start index should always be 0.");
    assert(firstSliceId.endIdx - firstSliceId.startIdx == 1 &&
           "Indirect stream should never merge slices.");
    MLC_SLICE_DPRINTF(firstSliceId, "Initial offset pop.\n");
    this->headSliceIdx++;
    this->slices.pop_front();
  }
}

void MLCDynamicIndirectStream::receiveStreamData(const ResponseMsg &msg) {
  MLC_S_DPRINTF("Indirect received stream data.\n");

  // It is indeed a problem to synchronize the flow control between
  // base stream and indirect stream.
  // It is possible for an indirect stream to receive stream data
  // beyond the tailSliceIdx, so we adhoc to fix that.
  // ! This breaks the MaximumNumElement constraint.

  while (this->tailSliceIdx <= msg.m_sliceId.startIdx) {
    this->allocateSlice();
  }

  MLCDynamicStream::receiveStreamData(msg);
}

void MLCDynamicIndirectStream::sendCreditToLLC() {
  // Just update the record.
  // Since the credit is managed through the base stream.
  this->llcTailSliceIdx = this->tailSliceIdx;
}