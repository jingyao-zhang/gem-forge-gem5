#include "MLCDynamicIndirectStream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/RubyStream.hh"

#define MLCS_DPRINTF(format, args...)                                          \
  DPRINTF(RubyStream, "[MLC_SE%d][%lu]: " format,                              \
          this->controller->getMachineID().num,                                \
          this->dynamicStreamId.staticId, ##args)

#define MLC_ELEMENT_DPRINTF(startIdx, numElements, format, args...)            \
  DPRINTF(RubyStream, "[MLC_SE%d][%lu][%lu, +%d): " format,                    \
          this->controller->getMachineID().num,                                \
          this->dynamicStreamId.staticId, (startIdx), (numElements), ##args)

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
    assert(!this->elements.empty() && "Empty initial elements list.");
    // Let's do some sanity check.
    auto &firstElement = this->elements.front();
    assert(firstElement.startIdx == 0 && "Start index should always be 0.");
    assert(firstElement.numElements == 1 &&
           "Indirect stream should never merge elements.");
    MLC_ELEMENT_DPRINTF(firstElement.startIdx, firstElement.numElements,
                        "Initial offset pop.\n");
    this->headIdx += firstElement.numElements;
    this->elements.pop_front();
  }
}

void MLCDynamicIndirectStream::receiveStreamData(const ResponseMsg &msg) {
  MLCS_DPRINTF("Indirect received stream data.\n");

  // It is indeed a problem to synchronize the flow control between
  // base stream and indirect stream.
  // It is possible for an indirect stream to receive stream data
  // beyond the tailIdx, so we adhoc to fix that.
  // ! This breaks the MaximumNumElement constraint.

  while (this->tailIdx <= msg.m_sliceId.startIdx) {
    this->allocateElement();
  }

  MLCDynamicStream::receiveStreamData(msg);
}

void MLCDynamicIndirectStream::sendCreditToLLC() {
  // Just update the record.
  this->llcTailIdx = this->tailIdx;
}