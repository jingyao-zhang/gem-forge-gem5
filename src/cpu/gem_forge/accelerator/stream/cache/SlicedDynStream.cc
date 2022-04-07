#include "SlicedDynStream.hh"

#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include "debug/SlicedDynStream.hh"
#include "mem/ruby/common/Address.hh"
#include "mem/ruby/system/RubySystem.hh"

#define DEBUG_TYPE SlicedDynStream
#include "../stream_log.hh"

SlicedDynStream::SlicedDynStream(CacheStreamConfigureDataPtr _configData)
    : strandId(_configData->dynamicId, _configData->strandIdx,
               _configData->totalStrands),
      formalParams(_configData->addrGenFormalParams),
      addrGenCallback(_configData->addrGenCallback),
      stepElemCount(_configData->stepElemCount),
      elemSize(_configData->elementSize), elemPerSlice(1),
      totalTripCount(_configData->totalTripCount),
      innerTripCount(_configData->innerTripCount),
      isPtChase(_configData->isPointerChase), ptrChaseState(_configData),
      tailElemIdx(0), sliceHeadElemIdx(0) {

  // Try to compute element per slice.
  if (auto linearAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
          this->addrGenCallback)) {
    auto innerStride = linearAddrGen->getInnerStride(this->formalParams);
    auto blockBytes = RubySystem::getBlockSizeBytes();
    if (innerStride <= blockBytes) {
      this->elemPerSlice = blockBytes / innerStride;
    } else {
      if (this->elemSize <= blockBytes) {
        this->elemPerSlice = 1.0f;
      } else {
        this->elemPerSlice =
            static_cast<float>(blockBytes) /
            static_cast<float>(
                std::min(static_cast<int64_t>(this->elemSize), innerStride));
        DYN_S_DPRINTF(
            this->strandId,
            "innerStride %lu elementSize %lu block %lu elementPerSlice %f.\n",
            innerStride, elemSize, blockBytes, this->elemPerSlice);
      }
    }
  }

  if (this->isPtChase || !_configData->shouldBeSlicedToCacheLines) {
    DYN_S_DPRINTF(this->strandId, "[Sliced] Disabled slicing.\n");
    this->coalesceContinuousElements = false;
    this->elemPerSlice = 1.0f;
    assert(this->elemSize < 64 && "Huge Non-Sliced StreamElem.");
  }

  if (_configData->floatPlan.getFirstFloatElementIdx() > 0) {
    auto firstFloatElemIdx = _configData->floatPlan.getFirstFloatElementIdx();
    DYN_S_DPRINTF(this->strandId, "[Sliced] Start from Element %llu.\n",
                  firstFloatElemIdx);

    this->tailElemIdx = firstFloatElemIdx;
    this->sliceHeadElemIdx = firstFloatElemIdx;
  }
}

SlicedDynStream::PtrChaseState::PtrChaseState(
    CacheStreamConfigureDataPtr &_configData) {
  if (!_configData->isPointerChase) {
    return;
  }
  this->memStream = _configData->stream;
  for (const auto &depEdge : _configData->depEdges) {
    const auto &depConfig = depEdge.data;
    if (depConfig->stream->isPointerChaseIndVar()) {
      this->ivStream = depConfig->stream;
      this->currentIVValue = depConfig->reductionInitValue;
      this->ivAddrFormalParams = depConfig->addrGenFormalParams;
      this->ivAddrGenCallback = depConfig->addrGenCallback;
      break;
    }
  }
  if (!this->ivStream) {
    DYN_S_PANIC(_configData->dynamicId, "Failed to get PtrChaseIV.");
  }
}

Addr SlicedDynStream::getOrComputePtrChaseElemVAddr(uint64_t elemIdx) const {
  auto &state = this->ptrChaseState;
  while (state.elementVAddrs.size() <= elemIdx &&
         !state.currentIVValueFaulted) {
    auto nextVAddr =
        this->addrGenCallback
            ->genAddr(elemIdx, this->formalParams,
                      GetSingleStreamValue(state.ivStream->staticId,
                                           state.currentIVValue))
            .front();
    if (makeLineAddress(nextVAddr + this->elemSize - 1) !=
        makeLineAddress(nextVAddr)) {
      DYN_S_PANIC(this->strandId,
                  "[PtrChase] Multi-Line Element %llu VAddr %#x.",
                  state.elementVAddrs.size(), nextVAddr);
    }
    state.elementVAddrs.push_back(nextVAddr);
    // Translate and get next IVValue.
    auto cpuDelegator = state.memStream->getCPUDelegator();
    Addr nextPAddr;
    if (cpuDelegator->translateVAddrOracle(nextVAddr, nextPAddr)) {
      DYN_S_DPRINTF(this->strandId, "[PtrChase] Generate %lluth VAddr %#x.\n",
                    state.elementVAddrs.size() - 1, nextVAddr);
      StreamValue nextMemValue;
      cpuDelegator->readFromMem(nextVAddr, this->elemSize,
                                nextMemValue.uint8Ptr());
      // Compute the NextIVValue.
      state.currentIVValue = state.ivAddrGenCallback->genAddr(
          elemIdx + 1, state.ivAddrFormalParams,
          GetCoalescedStreamValue(state.memStream, nextMemValue));
    } else {
      DYN_S_DPRINTF(this->strandId,
                    "[PtrChase] Generate %lluth Faulted VAddr %#x.\n",
                    state.elementVAddrs.size() - 1, nextVAddr);
      state.currentIVValueFaulted = true;
    }
  }
  if (elemIdx < state.elementVAddrs.size()) {
    return state.elementVAddrs.at(elemIdx);
  } else {
    return 0;
  }
}

void SlicedDynStream::stepTailElemIdx() const {
  this->prevTailElemIdx = this->tailElemIdx;
  this->tailElemIdx += this->stepElemCount;
}

DynStreamSliceId SlicedDynStream::getNextSlice() {
  while (slices.empty() || slices.front().getEndIdx() == this->tailElemIdx) {
    // Allocate until it's guaranteed that the first slice has no more
    // overlaps.
    this->allocateOneElement();
  }
  auto slice = this->slices.front();
  this->slices.pop_front();
  return slice;
}

const DynStreamSliceId &SlicedDynStream::peekNextSlice() const {
  while (slices.empty() || slices.front().getEndIdx() == this->tailElemIdx) {
    // Allocate until it's guaranteed that the first slice has no more
    // overlaps.
    this->allocateOneElement();
  }
  return this->slices.front();
}

void SlicedDynStream::setTotalAndInnerTripCount(int64_t tripCount) {
  if (this->hasTotalTripCount() || this->hasInnerTripCount()) {
    DYN_S_PANIC(this->strandId,
                "[Sliced] Reset TotalTripCount %ld InnerTripCount %ld -> %ld.",
                this->totalTripCount, this->innerTripCount, tripCount);
  }
  DYN_S_DPRINTF(this->strandId, "[Sliced] Set Total/InnerTripCount %lld.\n",
                tripCount);
  this->totalTripCount = tripCount;
  this->innerTripCount = tripCount;
}

Addr SlicedDynStream::getElementVAddr(uint64_t elementIdx) const {
  if (this->isPtChase) {
    return this->getOrComputePtrChaseElemVAddr(elementIdx);
  }
  return this->addrGenCallback
      ->genAddr(elementIdx, this->formalParams, getStreamValueFail)
      .front();
}

void SlicedDynStream::allocateOneElement() const {

  // Let's not worry about indirect streams here.
  const auto lhs = this->getElementVAddr(this->tailElemIdx);

  if (lhs + this->elemSize < lhs) {
    /**
     * This is a bug case when the vaddr wraps around. This is stream is
     * ill-defined and this is likely caused by misspeculation on StreamConfig.
     * This stream should soon be rewinded. Here I just make two new slices,
     * and do not bother coalescing with previous slices.
     */
    auto wrappedSize = lhs + this->elemSize;
    auto straightSize = this->elemSize - wrappedSize;
    assert(wrappedSize <= RubySystem::getBlockSizeBytes() &&
           "WrappedSize larger than a line.");
    assert(straightSize <= RubySystem::getBlockSizeBytes() &&
           "StraightSize larger than a line.");
    {
      // Straight slice.
      this->slices.emplace_back();
      auto &slice = this->slices.back();
      slice.getDynStrandId() = this->strandId;
      slice.getStartIdx() = this->tailElemIdx;
      slice.getEndIdx() = this->tailElemIdx + 1;
      slice.vaddr = makeLineAddress(lhs);
      slice.size = RubySystem::getBlockSizeBytes();
    }
    {
      // Wrapped slice.
      this->slices.emplace_back();
      auto &slice = this->slices.back();
      slice.getDynStrandId() = this->strandId;
      slice.getStartIdx() = this->tailElemIdx;
      slice.getEndIdx() = this->tailElemIdx + 1;
      slice.vaddr = makeLineAddress(0);
      slice.size = RubySystem::getBlockSizeBytes();
    }

    // Reset the sliceHeadElementIdx.
    this->sliceHeadElemIdx = this->tailElemIdx;
    this->stepTailElemIdx();
    return;
  }

  auto rhs = lhs + this->elemSize;
  auto prevLHS = this->tailElemIdx > 0
                     ? this->getElementVAddr(this->tailElemIdx - 1)
                     : lhs;
  bool prevWrappedAround = (prevLHS + this->elemSize) < prevLHS;

  // Break to cache line granularity, [lhsBlock, rhsBlock]
  auto lhsBlock = makeLineAddress(lhs);
  auto rhsBlock = makeLineAddress(rhs - 1);
  auto prevLHSBlock = makeLineAddress(prevLHS);
  assert(rhsBlock >= lhsBlock && "Wrapped around should be handled above.");

  DYN_S_DPRINTF(this->strandId,
                "Allocate elem %llu, vaddr [%#x, +%d), block [%#x, %#x].\n",
                this->tailElemIdx, lhs, this->elemSize, lhsBlock, rhsBlock);

  /**
   * Check if we can try to coalesce continuous elements.
   * Set the flag && not overflow && elements are continuous.
   */
  auto curBlock = lhsBlock;
  if (this->coalesceContinuousElements &&
      this->tailElemIdx == this->prevTailElemIdx + 1 &&
      !this->hasOverflowed(this->tailElemIdx)) {
    if (lhsBlock < prevLHSBlock && !prevWrappedAround) {
      /**
       * Special case to handle decreasing address.
       * If there is a bump back to lower address, we make sure that it has no
       * overlap with existing slices and restart.
       */
      for (auto &slice : this->slices) {
        assert(rhsBlock < slice.vaddr && "Overlapped decreasing element.");
      }
      // Set sliceHeadElementIdx so that slicing branch below will ignore
      // previous slices and restart.
      this->sliceHeadElemIdx = this->tailElemIdx;
    } else {
      // Non-decreasing case, keep going.
      // Update existing slices to the new element if there is overlap.
      for (auto &slice : this->slices) {
        if (slice.getStartIdx() < this->sliceHeadElemIdx) {
          // This slice is already "sealed" by a decreasing element.
          continue;
        }
        if (slice.vaddr == curBlock) {
          assert(slice.getEndIdx() == this->tailElemIdx &&
                 "Hole in overlapping elements.");
          slice.getEndIdx()++;
          curBlock += RubySystem::getBlockSizeBytes();
          if (curBlock > rhsBlock || curBlock < lhsBlock) {
            // We are done. If we wrapped around, then curBlock < lhsBlock.
            break;
          }
        }
      }
    }
  } else {
    // Simple case: no coalescing.
    // For sanity check: we update the sliceHeadElementIdx.
    this->sliceHeadElemIdx = this->tailElemIdx;
  }

  if (this->isPtChase) {
    /**
     * PointerChaseStream would not be sliced.
     */
    this->slices.emplace_back();
    auto &slice = this->slices.back();
    slice.getDynStrandId() = this->strandId;
    slice.getStartIdx() = this->tailElemIdx;
    slice.getEndIdx() = this->tailElemIdx + 1;
    slice.vaddr = lhs;
    slice.size = this->elemSize;
  } else {
    while (curBlock <= rhsBlock && curBlock >= lhsBlock) {
      this->slices.emplace_back();
      auto &slice = this->slices.back();
      slice.getDynStrandId() = this->strandId;
      slice.getStartIdx() = this->tailElemIdx;
      slice.getEndIdx() = this->tailElemIdx + 1;
      slice.vaddr = curBlock;
      slice.size = RubySystem::getBlockSizeBytes();
      curBlock += RubySystem::getBlockSizeBytes();
    }
  }

  this->stepTailElemIdx();
}
