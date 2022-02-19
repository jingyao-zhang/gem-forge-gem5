#include "SlicedDynStream.hh"

#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include "debug/SlicedDynStream.hh"
#include "mem/ruby/common/Address.hh"
#include "mem/ruby/system/RubySystem.hh"

#define DEBUG_TYPE SlicedDynStream
#include "../stream_log.hh"

SlicedDynStream::SlicedDynStream(CacheStreamConfigureDataPtr _configData)
    : streamId(_configData->dynamicId),
      formalParams(_configData->addrGenFormalParams),
      addrGenCallback(_configData->addrGenCallback),
      elementSize(_configData->elementSize), elementPerSlice(1),
      totalTripCount(_configData->totalTripCount),
      isPointerChase(_configData->isPointerChase), ptrChaseState(_configData),
      tailElementIdx(0), sliceHeadElementIdx(0) {

  // Try to compute element per slice.
  if (auto linearAddrGen = std::dynamic_pointer_cast<LinearAddrGenCallback>(
          this->addrGenCallback)) {
    auto innerStride = linearAddrGen->getInnerStride(this->formalParams);
    auto blockBytes = RubySystem::getBlockSizeBytes();
    if (innerStride <= blockBytes) {
      this->elementPerSlice = blockBytes / innerStride;
    } else {
      if (this->elementSize <= blockBytes) {
        this->elementPerSlice = 1.0f;
      } else {
        this->elementPerSlice =
            static_cast<float>(blockBytes) /
            static_cast<float>(
                std::min(static_cast<int64_t>(this->elementSize), innerStride));
        DYN_S_DPRINTF(
            this->streamId,
            "innerStride %lu elementSize %lu block %lu elementPerSlice %f.\n",
            innerStride, elementSize, blockBytes, this->elementPerSlice);
      }
    }
  }

  if (this->isPointerChase) {
    this->coalesceContinuousElements = false;
    assert(this->elementSize < 64 && "Huge PointerChaseStream.");
  }

  if (_configData->floatPlan.getFirstFloatElementIdx() > 0) {
    auto firstFloatElemIdx = _configData->floatPlan.getFirstFloatElementIdx();
    DYN_S_DPRINTF(this->streamId, "[Sliced] Start from Element %llu.\n",
                  firstFloatElemIdx);

    this->tailElementIdx = firstFloatElemIdx;
    this->sliceHeadElementIdx = firstFloatElemIdx;
  }
}

SlicedDynStream::PointerChaseState::PointerChaseState(
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

Addr SlicedDynStream::getOrComputePointerChaseElementVAddr(
    uint64_t elementIdx) const {
  auto &state = this->ptrChaseState;
  while (state.elementVAddrs.size() <= elementIdx &&
         !state.currentIVValueFaulted) {
    auto nextVAddr =
        this->addrGenCallback
            ->genAddr(elementIdx, this->formalParams,
                      GetSingleStreamValue(state.ivStream->staticId,
                                           state.currentIVValue))
            .front();
    if (makeLineAddress(nextVAddr + this->elementSize - 1) !=
        makeLineAddress(nextVAddr)) {
      DYN_S_PANIC(this->streamId,
                  "[PtrChase] Multi-Line Element %llu VAddr %#x.",
                  state.elementVAddrs.size(), nextVAddr);
    }
    state.elementVAddrs.push_back(nextVAddr);
    // Translate and get next IVValue.
    auto cpuDelegator = state.memStream->getCPUDelegator();
    Addr nextPAddr;
    if (cpuDelegator->translateVAddrOracle(nextVAddr, nextPAddr)) {
      DYN_S_DPRINTF(this->streamId, "[PtrChase] Generate %lluth VAddr %#x.\n",
                    state.elementVAddrs.size() - 1, nextVAddr);
      StreamValue nextMemValue;
      cpuDelegator->readFromMem(nextVAddr, this->elementSize,
                                nextMemValue.uint8Ptr());
      // Compute the NextIVValue.
      state.currentIVValue = state.ivAddrGenCallback->genAddr(
          elementIdx + 1, state.ivAddrFormalParams,
          GetCoalescedStreamValue(state.memStream, nextMemValue));
    } else {
      DYN_S_DPRINTF(this->streamId,
                    "[PtrChase] Generate %lluth Faulted VAddr %#x.\n",
                    state.elementVAddrs.size() - 1, nextVAddr);
      state.currentIVValueFaulted = true;
    }
  }
  if (elementIdx < state.elementVAddrs.size()) {
    return state.elementVAddrs.at(elementIdx);
  } else {
    return 0;
  }
}

DynStreamSliceId SlicedDynStream::getNextSlice() {
  while (slices.empty() || slices.front().getEndIdx() == this->tailElementIdx) {
    // Allocate until it's guaranteed that the first slice has no more
    // overlaps.
    this->allocateOneElement();
  }
  auto slice = this->slices.front();
  this->slices.pop_front();
  return slice;
}

const DynStreamSliceId &SlicedDynStream::peekNextSlice() const {
  while (slices.empty() || slices.front().getEndIdx() == this->tailElementIdx) {
    // Allocate until it's guaranteed that the first slice has no more
    // overlaps.
    this->allocateOneElement();
  }
  return this->slices.front();
}

void SlicedDynStream::setTotalTripCount(int64_t totalTripCount) {
  if (this->hasTotalTripCount()) {
    DYN_S_PANIC(this->streamId, "[Sliced] Reset TotalTripCount %lld -> %lld.",
                this->totalTripCount, totalTripCount);
  }
  DYN_S_DPRINTF(this->streamId, "[Sliced] Set TotalTripCount %lld.\n",
                totalTripCount);
  this->totalTripCount = totalTripCount;
}

Addr SlicedDynStream::getElementVAddr(uint64_t elementIdx) const {
  if (this->isPointerChase) {
    return this->getOrComputePointerChaseElementVAddr(elementIdx);
  }
  return this->addrGenCallback
      ->genAddr(elementIdx, this->formalParams, getStreamValueFail)
      .front();
}

void SlicedDynStream::allocateOneElement() const {

  // Let's not worry about indirect streams here.
  const auto lhs = this->getElementVAddr(this->tailElementIdx);

  if (lhs + this->elementSize < lhs) {
    /**
     * This is a bug case when the vaddr wraps around. This is stream is
     * ill-defined and this is likely caused by misspeculation on StreamConfig.
     * This stream should soon be rewinded. Here I just make two new slices,
     * and do not bother coalescing with previous slices.
     */
    auto wrappedSize = lhs + this->elementSize;
    auto straightSize = this->elementSize - wrappedSize;
    assert(wrappedSize <= RubySystem::getBlockSizeBytes() &&
           "WrappedSize larger than a line.");
    assert(straightSize <= RubySystem::getBlockSizeBytes() &&
           "StraightSize larger than a line.");
    {
      // Straight slice.
      this->slices.emplace_back();
      auto &slice = this->slices.back();
      slice.getDynStreamId() = this->streamId;
      slice.getStartIdx() = this->tailElementIdx;
      slice.getEndIdx() = this->tailElementIdx + 1;
      slice.vaddr = makeLineAddress(lhs);
      slice.size = RubySystem::getBlockSizeBytes();
    }
    {
      // Wrapped slice.
      this->slices.emplace_back();
      auto &slice = this->slices.back();
      slice.getDynStreamId() = this->streamId;
      slice.getStartIdx() = this->tailElementIdx;
      slice.getEndIdx() = this->tailElementIdx + 1;
      slice.vaddr = makeLineAddress(0);
      slice.size = RubySystem::getBlockSizeBytes();
    }

    // Reset the sliceHeadElementIdx.
    this->sliceHeadElementIdx = this->tailElementIdx;
    this->tailElementIdx++;
    return;
  }

  auto rhs = lhs + this->elementSize;
  auto prevLHS = this->tailElementIdx > 0
                     ? this->getElementVAddr(this->tailElementIdx - 1)
                     : lhs;
  bool prevWrappedAround = (prevLHS + this->elementSize) < prevLHS;

  // Break to cache line granularity, [lhsBlock, rhsBlock]
  auto lhsBlock = makeLineAddress(lhs);
  auto rhsBlock = makeLineAddress(rhs - 1);
  auto prevLHSBlock = makeLineAddress(prevLHS);
  assert(rhsBlock >= lhsBlock && "Wrapped around should be handled above.");

  DYN_S_DPRINTF(this->streamId,
                "Allocate element %llu, vaddr [%#x, +%d), block [%#x, %#x].\n",
                this->tailElementIdx, lhs, this->elementSize, lhsBlock,
                rhsBlock);

  /**
   * Check if we can try to coalesce continuous elements.
   * Set the flag && not overflow.
   */
  auto curBlock = lhsBlock;
  if (this->coalesceContinuousElements &&
      !this->hasOverflowed(this->tailElementIdx)) {
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
      this->sliceHeadElementIdx = this->tailElementIdx;
    } else {
      // Non-decreasing case, keep going.
      // Update existing slices to the new element if there is overlap.
      for (auto &slice : this->slices) {
        if (slice.getStartIdx() < this->sliceHeadElementIdx) {
          // This slice is already "sealed" by a decreasing element.
          continue;
        }
        if (slice.vaddr == curBlock) {
          assert(slice.getEndIdx() == this->tailElementIdx &&
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
    this->sliceHeadElementIdx = this->tailElementIdx;
  }

  if (this->isPointerChase) {
    /**
     * PointerChaseStream would not be sliced.
     */
    this->slices.emplace_back();
    auto &slice = this->slices.back();
    slice.getDynStreamId() = this->streamId;
    slice.getStartIdx() = this->tailElementIdx;
    slice.getEndIdx() = this->tailElementIdx + 1;
    slice.vaddr = lhs;
    slice.size = this->elementSize;
  } else {
    while (curBlock <= rhsBlock && curBlock >= lhsBlock) {
      this->slices.emplace_back();
      auto &slice = this->slices.back();
      slice.getDynStreamId() = this->streamId;
      slice.getStartIdx() = this->tailElementIdx;
      slice.getEndIdx() = this->tailElementIdx + 1;
      slice.vaddr = curBlock;
      slice.size = RubySystem::getBlockSizeBytes();
      curBlock += RubySystem::getBlockSizeBytes();
    }
  }

  this->tailElementIdx++;
}