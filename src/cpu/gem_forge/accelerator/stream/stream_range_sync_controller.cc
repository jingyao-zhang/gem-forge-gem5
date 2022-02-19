#include "stream_range_sync_controller.hh"

#include "debug/StreamRangeSync.hh"
#define DEBUG_TYPE StreamRangeSync
#include "stream_log.hh"

StreamRangeSyncController::StreamRangeSyncController(StreamEngine *_se)
    : se(_se) {}

DynStream *StreamRangeSyncController::getNoRangeDynS() {
  if (!this->se->isStreamRangeSyncEnabled()) {
    // We do not do range check.
    return nullptr;
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
      /**
       * There is no range for the next element, with exception for element
       * beyond the total trip count.
       *
       * Notice that our current implementation always check for the FIFOHead
       * + 1. This implicitly assumes all streams will be stepped. However, this
       * is not the case for two level of loop:
       *
       * config(s_i, s_j)
       * for i
       *   for j
       *     step i
       *   step j
       * end(s_i, s_j)
       *
       * The outer loop step will check for TotalTripCount + 1. So far we fix
       * this by ignore CheckElementIdx >= TotalTripCount.
       */
      if (dynS->hasTotalTripCount() &&
          this->getCheckElementIdx(dynS) >= dynS->getTotalTripCount()) {
        continue;
      }
      return dynS;
    }
  }
  return nullptr;
}

StreamRangeSyncController::DynStreamVec
StreamRangeSyncController::getCurrentDynStreams() {
  // Simply extract the head dynamic streams that offloaded as root.
  DynStreamVec dynStreams;
  for (auto &idStream : this->se->streamMap) {
    auto S = idStream.second;
    if (!S->hasDynStream()) {
      continue;
    }
    auto &dynS = S->getFirstDynStream();
    if (dynS.isFloatedToCacheAsRoot() && !dynS.isFloatConfigDelayed() &&
        dynS.shouldRangeSync()) {
      dynStreams.push_back(&S->getFirstDynStream());
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
        DYN_S_DPRINTF(dynS->dynStreamId,
                      "[CoreRange] Release range for element [%llu, +%d).\n",
                      currentWorkingRange->elementRange.getLHSElementIdx(),
                      currentWorkingRange->elementRange.getNumElements());
        dynS->setCurrentWorkingRange(nullptr);
      }
    }
    // Fill in next range if it's available.
    if (!dynS->getCurrentWorkingRange()) {
      /**
       * Since we do not check for the first element, it is possible that
       * we checking for element 1, but the first range we got is [0, 1).
       * We have to release old ranges here.
       * ! This should really be fixed after we can check for the first element.
       */
      while (auto nextRange = dynS->getNextReceivedRange()) {
        if (nextRange->elementRange.rhsElementIdx > elementIdx) {
          break;
        }
        // This range is old.
        dynS->popReceivedRange();
      }
      auto nextRange = dynS->getNextReceivedRange();
      if (nextRange && nextRange->elementRange.contains(elementIdx)) {

        // We want to fill in the next range,
        // but before that we have to check aliasing
        this->checkAliasBetweenRanges(dynStreams, nextRange);

        DYN_S_DPRINTF(dynS->dynStreamId,
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
    DynStreamVec &dynStreams, const DynStreamAddressRangePtr &newRange) {
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
      /**
       * We keep search in the each individual streams.
       */
      for (const auto &currentSubRange : currentRange->subRanges) {
        for (const auto &newSubRange : newRange->subRanges) {
          if (currentSubRange->vaddrRange.hasOverlap(newSubRange->vaddrRange)) {
            DYN_S_PANIC(dynS->dynStreamId,
                        "[CoreRange] Alias between remote vaddr ranges \n %s "
                        "\nand %s\n",
                        *currentRange, *newRange);
          }
        }
      }
    }
  }
}

uint64_t StreamRangeSyncController::getCheckElementIdx(DynStream *dynS) {
  // Get the first element.
  auto element = dynS->getFirstElement();
  if (!element) {
    DYN_S_PANIC(dynS->dynStreamId,
                "Missing FirstElement to perform range check.");
  }
  // Since we are stepping the current one, we should check range for next
  // element.
  auto elementIdx = element->FIFOIdx.entryIdx + 1;
  return elementIdx;
}