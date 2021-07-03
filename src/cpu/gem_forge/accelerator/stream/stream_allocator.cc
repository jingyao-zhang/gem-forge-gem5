#include "stream_region_controller.hh"

#include "base/trace.hh"
#include "debug/CoreStreamAlloc.hh"

#define DEBUG_TYPE CoreStreamAlloc
#include "stream_log.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamLoopBound, format, ##args)
#define SE_PANIC(format, args...)                                              \
  panic("[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)

void StreamRegionController::allocateElements(StaticRegion &staticRegion) {

  /**
   * We don't know if StreamEnd has already been dispatched for the last
   * DynamicRegion. Break if so.
   */
  if (!staticRegion.streams.front()->isConfigured()) {
    return;
  }

  /**
   * Try to allocate more elements for configured streams.
   * Set a target, try to make sure all streams reach this target.
   * Then increment the target.
   */
  // Make a copy of the StepRootStream.
  auto stepRootStreams = staticRegion.step.stepRootStreams;

  // Sort by the allocated size.
  std::sort(stepRootStreams.begin(), stepRootStreams.end(),
            [](Stream *SA, Stream *SB) -> bool {
              return SA->getAllocSize() < SB->getAllocSize();
            });

  for (auto stepRootStream : stepRootStreams) {

    /**
     * ! A hack here to delay the allocation if the back base stream has
     * ! not caught up.
     */
    auto maxAllocSize = stepRootStream->maxSize;
    if (!stepRootStream->backBaseStreams.empty()) {
      for (auto backBaseS : stepRootStream->backBaseStreams) {
        if (backBaseS->stepRootStream == stepRootStream) {
          // ! This is acutally a pointer chasing pattern.
          // ! No constraint should be enforced here.
          continue;
        }
        if (backBaseS->stepRootStream == nullptr) {
          // ! THis is actually a constant load.
          // ! So far ignore this dependence.
          continue;
        }
        if (backBaseS->getAllocSize() < maxAllocSize) {
          // The back base stream is lagging behind.
          // Reduce the maxAllocSize.
          maxAllocSize = backBaseS->getAllocSize();
        }
      }
    }

    /**
     * With the new NestStream, we have to search for the correct dynamic stream
     * to allocate for. It is the first DynamicStream that:
     * 1. StreamEnd not dispatched.
     * 2. StreamConfig executed.
     * 3. If has TotalTripCount, not all step streams has allocated all
     * elements.
     */
    const auto &stepStreams = se->getStepStreamList(stepRootStream);
    DynamicStream *allocatingStepRootDynS = nullptr;
    for (auto &stepRootDynS : stepRootStream->dynamicStreams) {
      if (!stepRootDynS.configExecuted) {
        // Configure not executed, can not allocate.
        break;
      }
      if (stepRootDynS.hasTotalTripCount()) {
        auto totalTripCount = stepRootDynS.getTotalTripCount();
        bool allStepStreamsAllocated = true;
        for (auto stepS : stepStreams) {
          auto &stepDynS = stepS->getDynamicStreamByInstance(
              stepRootDynS.dynamicStreamId.streamInstance);
          // DYN_S_DPRINTF(stepDynS.dynamicStreamId,
          //               "TotalTripCount %d, Next FIFOIdx %s.\n",
          //               totalTripCount, stepDynS.FIFOIdx);
          if (stepDynS.FIFOIdx.entryIdx < totalTripCount + 1) {
            allStepStreamsAllocated = false;
            break;
          }
        }
        if (allStepStreamsAllocated) {
          // All allocated, we can move to next one.
          continue;
        }
      } else {
        /**
         * Only skip this if we have no TotalTripCount. This is
         * because StreamEnd may be misspeculated. And we ended
         * using all the FIFO for the next nested dynamic stream.
         */
        if (stepRootDynS.endDispatched) {
          continue;
        }
      }
      // Found it.
      allocatingStepRootDynS = &stepRootDynS;
      break;
    }
    if (!allocatingStepRootDynS) {
      // Failed to find an allocating DynStream.
      S_DPRINTF(stepRootStream,
                "No Allocating DynStream, AllocSize %d MaxSize %d.\n",
                stepRootStream->getAllocSize(), stepRootStream->maxSize);
      continue;
    }
    /**
     * Limit the maxAllocSize with totalTripCount to avoid allocation beyond
     * StreamEnd. Condition: maxAllocSize > allocSize: originally we are trying
     * to allocate more.
     * ! We allow (totalTripCount + 1) elements as StreamEnd would consume one
     * ! element.
     */
    {
      auto allocSize = allocatingStepRootDynS->allocSize;
      if (allocatingStepRootDynS->hasTotalTripCount() &&
          maxAllocSize > allocSize) {
        auto nextEntryIdx = allocatingStepRootDynS->FIFOIdx.entryIdx;
        auto maxTripCount = allocatingStepRootDynS->getTotalTripCount() + 1;
        if (nextEntryIdx >= maxTripCount) {
          // We are already overflowed, set maxAllocSize to allocSize to stop
          // allocating. NOTE: This should not happen at all.
          maxAllocSize = allocSize;
        } else {
          maxAllocSize =
              std::min(maxAllocSize, (maxTripCount - nextEntryIdx) + allocSize);
        }
      }
      /**
       * For PointerChase streams, at most 4 elements per DynStream.
       */
      if (stepRootStream->isPointerChase()) {
        const int MaxElementPerPointerChaseDynStream = 4;
        if (allocSize + maxAllocSize > MaxElementPerPointerChaseDynStream) {
          maxAllocSize = (allocSize > MaxElementPerPointerChaseDynStream)
                             ? allocSize
                             : (MaxElementPerPointerChaseDynStream - allocSize);
          DYN_S_DPRINTF(allocatingStepRootDynS->dynamicStreamId,
                        "Limit MaxElement/DynPointerChaseStream. AllocSize %d "
                        "MaxAllocSize %d.\n",
                        allocSize, maxAllocSize);
        }
      }
    }

    DYN_S_DPRINTF(
        allocatingStepRootDynS->dynamicStreamId,
        "Allocating StepRootDynS AllocSize %d MaxSize %d MaxAllocSize %d.\n",
        stepRootStream->getAllocSize(), stepRootStream->maxSize, maxAllocSize);

    /**
     * We should try to limit maximum allocation per cycle cause I still see
     * some deadlock when one stream used all the FIFO entries orz.
     */
    const size_t MaxAllocationPerCycle = 4;
    for (size_t targetSize = 1, allocated = 0;
         targetSize <= maxAllocSize && se->hasFreeElement() &&
         allocated < MaxAllocationPerCycle;
         ++targetSize) {
      for (auto S : stepStreams) {
        assert(S->isConfigured() && "Try to allocate for unconfigured stream.");
        if (!se->hasFreeElement()) {
          S_DPRINTF(S, "No FreeElement.\n");
          break;
        }
        if (S->getAllocSize() >= S->maxSize) {
          S_DPRINTF(S, "Reached MaxAllocSize %d >= %d.\n", S->getAllocSize(),
                    S->maxSize);
          continue;
        }
        auto &dynS = S->getDynamicStreamByInstance(
            allocatingStepRootDynS->dynamicStreamId.streamInstance);
        if (dynS.allocSize >= targetSize) {
          DYN_S_DPRINTF(dynS.dynamicStreamId, "Reached TargetSize %d >= %d.\n",
                        dynS.allocSize, targetSize);
          continue;
        }
        if (!dynS.areNextBaseElementsAllocated()) {
          DYN_S_DPRINTF(dynS.dynamicStreamId,
                        "NextBaseElements not allocated.\n");
          continue;
        }
        if (S != stepRootStream) {
          if (S->getAllocSize() >= stepRootStream->getAllocSize()) {
            // It doesn't make sense to allocate ahead than the step root.
            DYN_S_DPRINTF(dynS.dynamicStreamId,
                          "Do not allocate %d beyond StepRootS %d.\n",
                          S->getAllocSize(), stepRootStream->getAllocSize());
            continue;
          }
          if (dynS.allocSize >= allocatingStepRootDynS->allocSize) {
            // It also doesn't make sense to allocate ahead than root dynS.
            DYN_S_DPRINTF(dynS.dynamicStreamId,
                          "Do not allocate %d beyond StepRootDynS %d.\n",
                          dynS.allocSize, allocatingStepRootDynS->allocSize);
            continue;
          }
        }
        DYN_S_DPRINTF(dynS.dynamicStreamId, "Allocate %d.\n", dynS.allocSize);
        se->allocateElement(dynS);
        allocated++;
      }
    }
  }
}
