#include "stream_range_sync_controller.hh"

#include "debug/StreamRangeSync.hh"
#define DEBUG_TYPE StreamRangeSync
#include "stream_log.hh"

StreamRangeSyncController::StreamRangeSyncController(StreamEngine *_se)
    : se(_se) {}

bool StreamRangeSyncController::areRangesReady() {
  if (!this->se->isStreamRangeSyncEnabled()) {
    // We do not do range check.
    return true;
  }
  /**
   * We first get all current active dynamic streams.
   * Update their current working ranges.
   * And then check they all have the current working range.
   */
  auto dynStreams = this->getCurrentDynStreams();
  this->updateCurrentWorkingRange(dynStreams);

  for (auto *dynS : dynStreams) {
    if (!dynS->getCurrentWorkingRange()) {
      // There is no range for the next element, with a special case if
      // we have reached the total trip count, as there is no next iteration.
      if (dynS->hasTotalTripCount() &&
          this->getCheckElementIdx(dynS) == dynS->getTotalTripCount()) {
        continue;
      }
      // DYN_S_DPRINTF(dynS->dynamicStreamId,
      //               "[CoreRange] Not ready for CheckElement %llu.\n",
      //               this->getCheckElementIdx(dynS));
      return false;
    }
  }
  return true;
}

StreamRangeSyncController::DynStreamVec
StreamRangeSyncController::getCurrentDynStreams() {
  // Simply extract the head dynamic streams that offloaded as root.
  DynStreamVec dynStreams;
  for (auto &idStream : this->se->streamMap) {
    auto S = idStream.second;
    if (!S->hasDynamicStream()) {
      continue;
    }
    auto &dynS = S->getFirstDynamicStream();
    if (dynS.offloadedToCacheAsRoot && !dynS.offloadConfigDelayed &&
        dynS.shouldRangeSync()) {
      dynStreams.push_back(&S->getFirstDynamicStream());
    }
  }
  return dynStreams;
}

void StreamRangeSyncController::updateCurrentWorkingRange(
    DynStreamVec &dynStreams) {
  for (auto &dynS : dynStreams) {
    auto elementIdx = this->getCheckElementIdx(dynS);
    // Release possible old range.
    if (auto currentWorkingRange = dynS->getCurrentWorkingRange()) {
      if (currentWorkingRange->elementRange.rhsElementIdx <= elementIdx) {
        DYN_S_DPRINTF(dynS->dynamicStreamId,
                      "[CoreRange] Release range for element [%llu, +%d).\n",
                      currentWorkingRange->elementRange.getLHSElementIdx(),
                      currentWorkingRange->elementRange.getNumElements());
        dynS->setCurrentWorkingRange(nullptr);
      }
    }
    // Fill in next range if it's available.
    if (!dynS->getCurrentWorkingRange()) {
      auto nextRange = dynS->getNextReceivedRange();
      if (nextRange && nextRange->elementRange.contains(elementIdx)) {

        // We want to fill in the next range,
        // but before that we have to check aliasing
        this->checkAliasBetweenRanges(dynStreams, nextRange);

        DYN_S_DPRINTF(dynS->dynamicStreamId,
                      "[CoreRange] Advance WorkingRange [%llu, +%d).\n",
                      nextRange->elementRange.getLHSElementIdx(),
                      nextRange->elementRange.getNumElements());
        dynS->setCurrentWorkingRange(nextRange);
        dynS->popReceivedRange();
      }
    }
  }
}

void StreamRangeSyncController::checkAliasBetweenRanges(
    DynStreamVec &dynStreams, const DynamicStreamAddressRangePtr &newRange) {
  for (auto &dynS : dynStreams) {
    auto currentRange = dynS->getCurrentWorkingRange();
    if (!currentRange) {
      continue;
    }
    /**
     * Normally we should check overlap in physical addresses. However,
     * for the current workloads, we never has two different virtual addresses
     * mapped to the same physical address. Therefore, to avoid the case of
     * false positive, I just check the vaddr range.
     */
    if (currentRange->vaddrRange.hasOverlap(newRange->vaddrRange)) {
      DYN_S_PANIC(
          dynS->dynamicStreamId,
          "[CoreRange] Alias between remote vaddr ranges \n %s \n and %s\n",
          *currentRange, *newRange);
    }
  }
}

uint64_t StreamRangeSyncController::getCheckElementIdx(DynamicStream *dynS) {
  // Get the first element.
  auto element = dynS->getFirstElement();
  if (!element) {
    DYN_S_PANIC(dynS->dynamicStreamId,
                "Missing FirstElement to perform range check.");
  }
  // Since we are stepping the current one, we should check range for next
  // element.
  auto elementIdx = element->FIFOIdx.entryIdx + 1;
  return elementIdx;
}