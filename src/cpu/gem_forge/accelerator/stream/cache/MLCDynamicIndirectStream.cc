#include "MLCDynamicIndirectStream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/RubyStream.hh"

#define MLCS_DPRINTF(format, args...)                                          \
  DPRINTF(RubyStream, "[MLC_SE%d][%lu]: " format,                              \
          this->controller->getMachineID().num,                                \
          this->dynamicStreamId.staticId, ##args)

MLCDynamicIndirectStream::MLCDynamicIndirectStream(
    CacheStreamConfigureData *_configData,
    AbstractStreamAwareController *_controller,
    MessageBuffer *_responseMsgBuffer, MessageBuffer *_requestToLLCMsgBuffer)
    : MLCDynamicStream(_configData, _controller, _responseMsgBuffer,
                       _requestToLLCMsgBuffer) {}

void MLCDynamicIndirectStream::receiveStreamData(const ResponseMsg &msg) {
  MLCS_DPRINTF("Indirect received stream data.\n");

  // It is indeed a problem to synchronize the flow control between
  // base stream and indirect stream.
  // It is possible for an indirect stream to receive stream data
  // beyond the tailIdx, so we adhoc to fix that.
  // ! This breaks the MaximumNumElement constraint.

  while (this->tailIdx <= msg.m_streamMeta.m_startIdx) {
    this->allocateElement();
  }

  MLCDynamicStream::receiveStreamData(msg);
}

void MLCDynamicIndirectStream::allocateElement() {
  auto historySize = this->history->history_size();
  uint64_t startIdx = this->tailIdx;
  int numElements = 1;
  Addr vaddr =
      (startIdx < historySize) ? (this->history->history(startIdx).addr()) : 0;
  this->tailIdx++;

  MLCS_DPRINTF("Allocate [%lu, %lu).\n", startIdx, startIdx + numElements);
  this->elements.emplace_back(startIdx, numElements, vaddr);
}

void MLCDynamicIndirectStream::sendCreditToLLC() {
  // Just update the record.
  this->llcTailIdx = this->tailIdx;
}