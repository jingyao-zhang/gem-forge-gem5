#include "MLCDynIndirectStream.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStreamBase.hh"
#include "debug/MLCRubyStreamReduce.hh"

#define DEBUG_TYPE MLCRubyStreamBase
#include "../stream_log.hh"

namespace gem5 {

MLCDynIndirectStream::MLCDynIndirectStream(
    CacheStreamConfigureDataPtr _configData,
    ruby::AbstractStreamAwareController *_controller,
    ruby::MessageBuffer *_responseMsgBuffer,
    ruby::MessageBuffer *_requestToLLCMsgBuffer,
    const DynStreamId &_rootStreamId)
    : MLCDynStream(_configData, _controller, _responseMsgBuffer,
                   _requestToLLCMsgBuffer, false /* isMLCDirect */),
      rootStreamId(_rootStreamId),
      formalParams(_configData->addrGenFormalParams),
      addrGenCallback(_configData->addrGenCallback),
      elementSize(_configData->elementSize),
      isOneIterationBehind(_configData->isOneIterationBehind),
      tailElementIdx(0) {}

void MLCDynIndirectStream::setBaseStream(MLCDynStream *baseStream) {
  assert(!this->baseStream && "Already has base stream.");
  this->baseStream = baseStream;

  for (const auto &edge : this->getConfig()->baseEdges) {
    if (edge.isUsedBy) {
      /**
       * ! For two-level indirect streams, this is not true.
       * ! They are connected to the DirectS as the BaseS.
       */
      // if (edge.dynStreamId != baseStream->getDynStreamId()) {
      //   MLC_S_PANIC_NO_DUMP(this->getDynStrandId(),
      //                       "Mismatch MLCBaseS %s != %s.", edge.dynStreamId,
      //                       baseStream->getDynStrandId());
      // }
      assert(edge.skip == 0 && "IndirectS with Non-Zero skip.");
      this->baseStreamReuse = edge.reuse;
      break;
    }
  }
}

void MLCDynIndirectStream::receiveStreamData(const DynStreamSliceId &sliceId,
                                             const ruby::DataBlock &dataBlock,
                                             Addr paddrLine, bool isAck) {

  // It is indeed a problem to synchronize the flow control between
  // base stream and indirect stream.
  // It is possible for an indirect stream to receive stream data
  // beyond the tailElementIdx, so we adhoc to fix that.
  // ! This breaks the MaximumNumElement constraint.

  MLC_SLICE_DPRINTF(sliceId, "Recv data vaddr %#x paddrLine %#x.\n",
                    sliceId.vaddr, paddrLine);

  // Intercept the reduction value.
  if (!isAck &&
      this->receiveFinalReductionValue(sliceId, dataBlock, paddrLine)) {
    return;
  }

  while (this->tailElementIdx <= sliceId.getStartIdx()) {
    this->allocateSlice();
  }

  assert(sliceId.isValid() && "Invalid stream slice id for stream data.");
  assert(this->strandId == sliceId.getDynStrandId() &&
         "Unmatched dynamic stream id.");

  auto numElements = sliceId.getNumElements();
  assert(numElements == 1 && "Can not merge indirect elements.");

  /**
   * It is possible when the core stream engine runs ahead than
   * the LLC stream engine, and the stream data is delivered after
   * the slice is released. In such case we will ignore the
   * stream data.
   *
   * TODO: Properly handle this with sliceIdx.
   */
  if (this->slices.empty()) {
    // The data arrived after the slices is released.
    MLC_SLICE_DPRINTF(sliceId, "Received Data. No Slice and Overflowed? %d.",
                      this->hasOverflowed());
    return;
  } else {
    /**
     * Check if the data is lagging behind. Since we guarantee that
     * all slices will be created during allocation, it is impossible that
     * we may need to insert new slices here.
     */
    auto frontVAddr = this->slices.front().sliceId.vaddr;
    auto frontElementIdx = this->slices.front().sliceId.getStartIdx();
    if (sliceId.getStartIdx() < frontElementIdx) {
      // Definitely behind.
      return;
    } else if (sliceId.getStartIdx() == frontElementIdx) {
      if (!this->isWaitingAck() && sliceId.vaddr < frontVAddr) {
        // Still behind.
        return;
      } else {
        // For stream waiting for Ack, there is no address associated with addr,
        // so we do not check for that.
      }
    }
  }

  /**
   * Find the correct stream slice and insert the data there.
   */
  auto elemSlices = this->findSliceByElementIdx(sliceId.getStartIdx());
  auto slicesBegin = elemSlices.first;
  auto slicesEnd = elemSlices.second;

  auto sliceIter = slicesBegin;
  // We only check for the address if we are not waiting for Ack.
  if (!this->isWaitingAck()) {
    auto targetLineAddr = ruby::makeLineAddress(sliceId.vaddr);
    while (sliceIter != slicesEnd) {
      assert(sliceIter->sliceId.getStartIdx() == sliceId.getStartIdx());
      auto sliceLineAddr = ruby::makeLineAddress(sliceIter->sliceId.vaddr);
      if (sliceLineAddr == targetLineAddr) {
        break;
      } else if (sliceLineAddr < targetLineAddr) {
        ++sliceIter;
      } else {
        MLC_SLICE_PANIC(sliceId, "Failed to find slice %#x.\n", targetLineAddr);
      }
    }
  }
  if (sliceIter == slicesEnd) {
    MLC_SLICE_PANIC(sliceId, "Failed to find slice.\n");
  }

  if (!sliceIter->dataReady) {
    sliceIter->setData(dataBlock, this->controller->curCycle());
  } else {
    // The only exception is the second Ack for RangeSync.
    if (!(this->shouldRangeSync() &&
          this->getStaticStream()->isAtomicComputeStream())) {
      MLC_SLICE_PANIC(sliceId, "Receive StreamData twice.");
    }
  }
  if (sliceIter->coreStatus == MLCStreamSlice::CoreStatusE::WAIT_DATA) {
    // Sanity check that LLC and Core generated the same address.
    // ! Core is line address.
    if (sliceIter->coreSliceId.vaddr != ruby::makeLineAddress(sliceId.vaddr)) {
      MLC_SLICE_PANIC(sliceId, "Mismatch between Core %#x and LLC %#x.\n",
                      sliceIter->coreSliceId.vaddr, sliceId.vaddr);
    }
    this->makeResponse(*sliceIter);
  } else if (sliceIter->coreStatus == MLCStreamSlice::CoreStatusE::WAIT_ACK) {
    // Ack the stream element.
    // TODO: Send the packet back via normal message buffer.
    // hack("Indirect slices acked element %llu size %llu header %llu.\n",
    //      sliceId.getStartIdx(), this->slices.size(),
    //      this->slices.front().sliceId.getStartIdx());
    this->makeAck(*sliceIter);
  } else if (sliceIter->coreStatus == MLCStreamSlice::CoreStatusE::ACK_READY) {
    MLC_SLICE_PANIC(sliceId, "Received multiple acks.");
  }
  this->advanceStream();
}

void MLCDynIndirectStream::fillElemVAddr(uint64_t strandElemIdx,
                                         GetStreamValueFunc &getBaseData) {

  // It's possible that we are behind the base stream?
  while (this->tailElementIdx <= strandElemIdx) {
    this->allocateSlice();
  }

  if (this->isWaitingNothing() || this->isWaitingAck()) {
    // If we are waiting for nothing, this is the only place we advance.
    MLC_S_DPRINTF(this->strandId, "WaitNothing/Ack. Skip FillElemVAddr.\n");
    return;
  }

  if (this->slices.empty()) {
    // We better be overflowed, unless we are Reduction.
    if (!this->getStaticStream()->isReduction()) {
      assert(this->hasOverflowed() && "No slices when not overflowed.");
    }
    return;
  } else {
    if (strandElemIdx < this->slices.front().sliceId.getStartIdx()) {
      // The stream is lagging behind the core. The slice has already been
      // released.
      return;
    }
  }

  auto elemVAddr = this->genElemVAddr(strandElemIdx, getBaseData);
  auto elemSize = this->elementSize;

  DynStreamSliceId sliceId;
  sliceId.getDynStrandId() = this->strandId;
  sliceId.getStartIdx() = strandElemIdx;
  sliceId.getEndIdx() = strandElemIdx + 1;

  // Search for the slices with the elementIdx.
  auto elementSlices = this->findSliceByElementIdx(strandElemIdx);
  auto slicesBegin = elementSlices.first;
  auto slicesEnd = elementSlices.second;

  // Iterate through elementSlices.
  auto totalSliceSize = 0;
  while (totalSliceSize < elemSize) {
    Addr curSliceVAddr = elemVAddr + totalSliceSize;
    // Make sure the slice is contained within one line.
    int lineOffset = curSliceVAddr % ruby::RubySystem::getBlockSizeBytes();
    auto curSliceSize = std::min(
        elemSize - totalSliceSize,
        static_cast<int>(ruby::RubySystem::getBlockSizeBytes()) - lineOffset);
    // Here we set the slice vaddr and size.
    sliceId.vaddr = curSliceVAddr;
    sliceId.size = curSliceSize;
    // Find the slice.
    auto sliceIter =
        this->findOrInsertSliceBySliceId(slicesBegin, slicesEnd, sliceId);

    MLC_SLICE_DPRINTF(sliceId,
                      "Processing totalSize %d elementSize %d vaddr %#x.\n",
                      totalSliceSize, elemSize, curSliceVAddr);

    /**
     * The reason we do this is to check if the indirect stream faulted,
     * in which case the LLC won't send data here, and we need to mark
     * this slice Fault and release it.
     */
    auto cpuDelegator = this->getStaticStream()->getCPUDelegator();
    Addr curSlicePAddr;
    if (!cpuDelegator->translateVAddrOracle(curSliceVAddr, curSlicePAddr)) {
      // This slice has faulted.
      sliceIter->coreStatus = MLCStreamSlice::CoreStatusE::FAULTED;
      MLC_SLICE_DPRINTF(sliceIter->sliceId,
                        "Faulted Indirect Element VAddr %#x.\n",
                        sliceIter->sliceId.vaddr);
    }
    totalSliceSize += curSliceSize;
  }
}

void MLCDynIndirectStream::mapBaseElemToMyElemIdxRange(
    uint64_t baseStrandElemIdx, uint64_t baseStreamElemIdx,
    uint64_t &streamElemIdxLhs, uint64_t &strandElemIdxLhs,
    DynStrandId &strandIdLhs, uint64_t &streamElemIdxRhs,
    uint64_t &strandElemIdxRhs, DynStrandId &strandIdRhs) const {

  auto reuse = this->getBaseStreamReuse();
  auto skip = this->getBaseStreamSkip();

  streamElemIdxLhs =
      this->config->convertBaseToDepElemIdx(baseStreamElemIdx, reuse, skip);
  streamElemIdxRhs =
      this->config->convertBaseToDepElemIdx(baseStreamElemIdx + 1, reuse, skip);

  // Decrement Rhs to make sure it's within the same strand.
  assert(streamElemIdxRhs > streamElemIdxLhs);
  streamElemIdxRhs--;

  strandElemIdxLhs =
      this->config->getStrandElemIdxFromStreamElemIdx(streamElemIdxLhs);
  strandElemIdxRhs =
      this->config->getStrandElemIdxFromStreamElemIdx(streamElemIdxRhs);

  strandIdLhs = this->config->getStrandIdFromStreamElemIdx(streamElemIdxLhs);
  strandIdRhs = this->config->getStrandIdFromStreamElemIdx(streamElemIdxRhs);

  MLC_S_DPRINTF(this->strandId,
                "IndS MapBase %d-%lu(%lu) Reuse %d My Lhs %d-%lu(%lu) Rhs "
                "%d-%lu(%lu).\n",
                this->baseStream->getDynStrandId().strandIdx, baseStrandElemIdx,
                baseStreamElemIdx, reuse, strandIdLhs.strandIdx,
                strandElemIdxLhs, streamElemIdxLhs, strandIdRhs.strandIdx,
                strandElemIdxRhs, streamElemIdxRhs);

  assert(strandIdLhs == strandIdRhs &&
         "IndirectS with Reuse Splitted into Multiple Strands.");
  assert(strandIdLhs == this->strandId && "Mismatch IndStrandId.");
}

void MLCDynIndirectStream::receiveBaseStreamData(
    uint64_t baseStrandElemIdx, GetStreamValueFunc &getBaseData,
    bool tryAdvance) {

  MLC_S_DPRINTF(this->strandId,
                "Recv BaseStreamData BaseStrandElemIdx %lu "
                "TailSliceIdx %lu TailElemIdx %lu.\n",
                baseStrandElemIdx, this->tailSliceIdx, this->tailElementIdx);

  auto baseStreamElemIdx =
      this->baseStream->getConfig()->getStreamElemIdxFromStrandElemIdx(
          baseStrandElemIdx);

  uint64_t myStreamElemIdxLhs;
  uint64_t myStrandElemIdxLhs;
  DynStrandId myStrandIdLhs;
  uint64_t myStreamElemIdxRhs;
  uint64_t myStrandElemIdxRhs;
  DynStrandId myStrandIdRhs;

  this->mapBaseElemToMyElemIdxRange(baseStrandElemIdx, baseStreamElemIdx,
                                    myStreamElemIdxLhs, myStrandElemIdxLhs,
                                    myStrandIdLhs, myStreamElemIdxRhs,
                                    myStrandElemIdxRhs, myStrandIdRhs);

  for (uint64_t strandElemIdx = myStrandElemIdxLhs;
       strandElemIdx <= myStrandElemIdxRhs; ++strandElemIdx) {
    this->fillElemVAddr(strandElemIdx, getBaseData);
  }

  if (this->isWaitingNothing() && tryAdvance) {
    // If we are waiting for nothing, this is the only place we advance.
    MLC_S_DPRINTF(this->strandId, "WaitNothing. Advance.\n");
    this->advanceStream();
  }
}

void MLCDynIndirectStream::advanceStream() {
  this->tryPopStream();

  /**
   * Do not try to allocate if the LLCElement is not created yet.
   * This is to avoid running ahead than the BaseS, which is in charge of
   * allocating LLCElement.
   */
  auto maxAllocElemIdx = this->tailElementIdx + this->maxNumSlices;
  if (auto llcDynS = LLCDynStream::getLLCStream(this->getDynStrandId())) {
    MLC_S_DPRINTF(this->getDynStrandId(),
                  "Adjust MaxAllocElemIdx = Min(%lu, LLC %lu).\n",
                  maxAllocElemIdx, llcDynS->getNextInitElementIdx());
    maxAllocElemIdx =
        std::min(maxAllocElemIdx, llcDynS->getNextInitElementIdx());
  }

  // Of course we need to allocate more slices.
  while (this->tailSliceIdx - this->headSliceIdx < this->maxNumSlices &&
         !this->hasOverflowed()) {
    if (this->tailElementIdx >= maxAllocElemIdx) {
      MLC_S_DPRINTF(this->getDynStrandId(),
                    "CanNot Allocate Element %llu, MaxAlloc %llu.\n",
                    this->tailElementIdx, maxAllocElemIdx);
      break;
    }
    this->allocateSlice();
  }

  // We may need to schedule advance stream if the first slice is FAULTED,
  // as no other event will cause it to be released.
  if (!this->slices.empty()) {
    auto frontCoreStatus = this->slices.front().coreStatus;
    if (frontCoreStatus == MLCStreamSlice::CoreStatusE::FAULTED ||
        frontCoreStatus == MLCStreamSlice::CoreStatusE::DONE) {
      this->scheduleAdvanceStream();
    }
  }

  // Let's try to schedule advanceStream for the baseStream.
  this->baseStream->scheduleAdvanceStream();
}

void MLCDynIndirectStream::allocateSlice() {
  // For indirect stream, there is no merging, so it's pretty simple
  // to allocate new slice.
  DynStreamSliceId sliceId;
  sliceId.getDynStrandId() = this->strandId;
  sliceId.getStartIdx() = this->tailElementIdx;
  sliceId.getEndIdx() = this->tailElementIdx + 1;

  MLC_SLICE_DPRINTF(sliceId, "Allocated indirect slice.\n");

  this->slices.emplace_back(sliceId);
  this->stream->statistic.numMLCAllocatedSlice++;

  auto &slice = this->slices.back();

  /**
   * The core still wait for ack to synchronize with the LLC. So far this is
   * done with hack. It would not issue request for reduction stream.
   */
  if (this->isWaitingAck()) {
    // Except the first element if we are one iter behind.
    if (this->isOneIterationBehind && sliceId.getStartIdx() == 0) {
      slice.coreStatus = MLCStreamSlice::CoreStatusE::DONE;
    } else {
      slice.coreStatus = MLCStreamSlice::CoreStatusE::WAIT_ACK;
    }
  } else if (this->isWaitingNothing()) {
    slice.coreStatus = MLCStreamSlice::CoreStatusE::DONE;
  }

  this->tailSliceIdx++;
  this->tailElementIdx++;
}

bool MLCDynIndirectStream::hasOverflowed() const {
  return this->hasTotalTripCount() &&
         this->tailElementIdx > this->getTotalTripCount();
}

void MLCDynIndirectStream::setTotalTripCount(
    int64_t totalTripCount, Addr brokenPAddr,
    ruby::MachineType brokenMachineType) {
  MLC_S_PANIC(this->getDynStrandId(), "Set TotalTripCount for IndirectS.");
}

Addr MLCDynIndirectStream::genElemVAddr(uint64_t strandElemIdx,
                                        GetStreamValueFunc &getBaseData) {

  auto getBaseStreamValue = [this, strandElemIdx,
                             &getBaseData](uint64_t streamId) -> StreamValue {
    if (this->baseStream->getStaticStream()->isCoalescedHere(streamId)) {
      return getBaseData(streamId);
    }

    /**
     * Search through UsedAffineIV to find the value.
     */
    for (const auto &edge : this->getConfig()->baseEdges) {
      if (!edge.isUsedAffineIV) {
        continue;
      }
      const auto &affineIVConfig = edge.usedAffineIV;
      if (!affineIVConfig->stream->isCoalescedHere(streamId)) {
        continue;
      }
      auto streamElemIdx =
          this->getConfig()->getStreamElemIdxFromStrandElemIdx(strandElemIdx);
      auto baseStreamElemIdx = this->getConfig()->convertDepToBaseElemIdx(
          streamElemIdx, edge.reuse, edge.skip);
      auto value = affineIVConfig->addrGenCallback->genAddr(
          baseStreamElemIdx, affineIVConfig->addrGenFormalParams,
          getStreamValueFail);

      MLC_S_DPRINTF(this->getDynStrandId(),
                    "Gen AffineIV %s %lu/%lu R/S %d/%d -> %lu Value %lu.\n",
                    edge.dynStreamId, strandElemIdx, streamElemIdx, edge.reuse,
                    edge.skip, baseStreamElemIdx, value.uint64());
      return value;
    }

    MLC_S_PANIC(this->getDynStrandId(), "Invalid BaseStreamId %llu.", streamId);
  };

  return this->addrGenCallback
      ->genAddr(strandElemIdx, this->formalParams, getBaseStreamValue)
      .front();
}

std::pair<MLCDynIndirectStream::SliceIter, MLCDynIndirectStream::SliceIter>
MLCDynIndirectStream::findSliceByElementIdx(uint64_t elementIdx) {
  auto ret = std::make_pair<SliceIter, SliceIter>(this->slices.end(),
                                                  this->slices.end());
  // hack("findSliceByElementIdx when slices are %d.\n", this->slices.size());
  for (auto iter = this->slices.begin(), end = this->slices.end(); iter != end;
       ++iter) {
    auto lhsElementIdx = iter->sliceId.getStartIdx();
    if (lhsElementIdx == elementIdx && ret.first == end) {
      // Find lhs.
      ret.first = iter;
    } else if (lhsElementIdx > elementIdx && ret.second == end) {
      // Find rhs.
      ret.second = iter;
      break;
    }
  }

  if (ret.first == this->slices.end()) {
    MLC_S_PANIC(this->getDynStrandId(),
                "Failed to find slice for Element %llu.\n", elementIdx);
  }
  return ret;
}

MLCDynIndirectStream::SliceIter
MLCDynIndirectStream::findOrInsertSliceBySliceId(
    const SliceIter &begin, const SliceIter &end,
    const DynStreamSliceId &sliceId) {
  auto ret = begin;
  if (ret->sliceId.vaddr == 0) {
    // This first slice has not been used. directly use it.
    assert(ret->sliceId.getStartIdx() == sliceId.getStartIdx() &&
           "Invalid elementIdx.");
    ret->sliceId.vaddr = sliceId.vaddr;
    return ret;
  }
  auto targetLineAddr = ruby::makeLineAddress(sliceId.vaddr);
  while (ret != end) {
    assert(ret->sliceId.getStartIdx() == sliceId.getStartIdx() &&
           "Invalid elementIdx.");
    auto lineAddr = ruby::makeLineAddress(ret->sliceId.vaddr);
    if (lineAddr == targetLineAddr) {
      // We found it.
      return ret;
    } else if (lineAddr < targetLineAddr) {
      // Keep searching.
      ++ret;
    } else {
      // This should be our next slice.
      break;
    }
  }
  // Insert one slice before ret.
  ret = this->slices.emplace(ret, sliceId);
  this->tailSliceIdx++;
  this->stream->statistic.numMLCAllocatedSlice++;
  MLC_SLICE_DPRINTF(ret->sliceId, "Insert indirect slice.\n");
  return ret;
}

MLCDynStream::SliceIter
MLCDynIndirectStream::findSliceForCoreRequest(const DynStreamSliceId &sliceId) {

  if (this->slices.empty()) {
    MLC_S_PANIC(this->strandId,
                "No slices for request, overflowed %d, totalTripCount %lu.\n",
                this->hasOverflowed(), this->getTotalTripCount());
  }

  // We try to allocate slices.
  auto elementSlices = this->findSliceByElementIdx(sliceId.getStartIdx());
  return this->findOrInsertSliceBySliceId(elementSlices.first,
                                          elementSlices.second, sliceId);
}

bool MLCDynIndirectStream::receiveFinalReductionValue(
    const DynStreamSliceId &sliceId, const ruby::DataBlock &dataBlock,
    Addr paddrLine) {
  auto S = this->getStaticStream();
  if (!(S->isReduction() || S->isPointerChaseIndVar())) {
    // Only Reduction/PtrChaseIV need final value.
    return false;
  }
  if (this->isWaitingData()) {
    // We are expecting the data value for every element.
    return false;
  }

  auto dynCoreS = this->getCoreDynS();
  if (!dynCoreS) {
    MLC_SLICE_DPRINTF_(
        MLCRubyStreamReduce, sliceId,
        "CoreDynS released before receiving FinalReductionValue.");
    return true;
  }
  auto size = S->getCoreElementSize();
  StreamValue value;
  memcpy(value.uint8Ptr(), dataBlock.getData(0, size), size);

  auto finalElemIdx = sliceId.getStartIdx();
  if (this->config->pumPartialReducedElems > 0) {
    /**
     * We basically assume that ReduceS is scalar version.
     */
    finalElemIdx *= this->config->pumPartialReducedElems;
  }

  dynCoreS->setInnerFinalValue(finalElemIdx, value);

  MLC_SLICE_DPRINTF_(MLCRubyStreamReduce, sliceId,
                     "Notify final reduction elem %llu.\n", finalElemIdx);

  return true;
}

void MLCDynIndirectStream::sample() const { MLCDynStream::sample(); }

} // namespace gem5
