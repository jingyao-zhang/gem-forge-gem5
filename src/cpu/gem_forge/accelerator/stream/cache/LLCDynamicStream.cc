#include "LLCDynamicStream.hh"

#include "cpu/gem_forge/accelerator/stream/stream.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "debug/LLCRubyStream.hh"
#define DEBUG_TYPE LLCRubyStream
#include "../stream_log.hh"

// TODO: Support real flow control.
LLCDynamicStream::LLCDynamicStream(AbstractStreamAwareController *_controller,
                                   CacheStreamConfigureData *_configData)
    : controller(_controller), configData(*_configData),
      slicedStream(_configData, true /* coalesceContinuousElements */),
      maxWaitingDataBaseRequests(2), sliceIdx(0),
      allocatedSliceIdx(_configData->initAllocatedIdx),
      waitingDataBaseRequests(0) {
  if (this->configData.isPointerChase) {
    // Pointer chase stream can only have at most one base requests waiting for
    // data.
    this->maxWaitingDataBaseRequests = 1;
  }
}

LLCDynamicStream::~LLCDynamicStream() {
  for (auto &indirectStream : this->indirectStreams) {
    delete indirectStream;
    indirectStream = nullptr;
  }
  this->indirectStreams.clear();
}

Addr LLCDynamicStream::peekVAddr() {
  return this->slicedStream.peekNextSlice().vaddr;
}

Addr LLCDynamicStream::getVAddr(uint64_t sliceIdx) const {
  panic("getVAddr is deprecated.\n");
  return 0;
}

bool LLCDynamicStream::translateToPAddr(Addr vaddr, Addr &paddr) const {
  // ! Do something reasonable here to translate the vaddr.
  auto cpuDelegator = this->configData.stream->getCPUDelegator();
  return cpuDelegator->translateVAddrOracle(vaddr, paddr);
}

void LLCDynamicStream::addCredit(uint64_t n) {
  this->allocatedSliceIdx += n;
  for (auto indirectStream : this->indirectStreams) {
    indirectStream->addCredit(n);
  }
}

void LLCDynamicStream::updateIssueClearCycle() {
  const auto *dynS =
      this->configData.stream->getDynamicStream(this->configData.dynamicId);
  if (dynS == nullptr) {
    // The dynS is already released.
    return;
  }
  auto avgTurnAroundCycle = dynS->getAvgTurnAroundCycle();
  auto avgLateElements = dynS->getNumLateElement();
  uint64_t newIssueClearCycle = this->issueClearCycle;
  if (avgTurnAroundCycle != 0) {
    // We need to adjust the turn around cycle from element to slice.
    uint64_t avgSliceTurnAroundCycle =
        static_cast<float>(avgTurnAroundCycle) * this->slicedStream.getElementPerSlice();
    // We divide by 1.5 so to reflect that we should be slighly faster than
    // core.
    uint64_t adjustSliceTurnAroundCycle = avgSliceTurnAroundCycle * 2 / 3;
    if (avgLateElements >= 2) {
      // If we have late elements, try to issue faster.
      newIssueClearCycle = std::max(1ul, this->issueClearCycle / 2);
    } else {
      newIssueClearCycle = adjustSliceTurnAroundCycle;
    }
    if (newIssueClearCycle != this->issueClearCycle) {
      LLC_S_DPRINTF(this->configData.dynamicId,
                    "Update IssueClearCycle %lu -> %lu, avgEleTurn %lu, "
                    "avgSliceTurn %lu, avgLateEle %d, elementPerSlice %f.\n",
                    this->issueClearCycle, newIssueClearCycle,
                    avgTurnAroundCycle, avgSliceTurnAroundCycle,
                    avgLateElements, this->slicedStream.getElementPerSlice());
      this->issueClearCycle = Cycles(newIssueClearCycle);
    }
  }
}