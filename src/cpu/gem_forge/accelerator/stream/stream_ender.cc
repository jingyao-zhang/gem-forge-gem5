#include "stream_region_controller.hh"

#include "stream_float_controller.hh"

#include "base/trace.hh"
#include "debug/StreamEnd.hh"

#define DEBUG_TYPE StreamEnd
#include "stream_log.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamEnd, format, ##args)
#define SE_PANIC(format, args...)                                              \
  panic("[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)

bool StreamRegionController::canDispatchStreamEnd(const EndArgs &args) {
  const auto &streamRegion = this->se->getStreamRegion(args.infoRelativePath);
  auto &staticRegion = this->getStaticRegion(streamRegion.region());

  auto dynRegion = this->tryGetFirstAliveDynRegion(staticRegion);
  if (!dynRegion) {
    // It's possible that the Stream has not configured yet (e.g., Nest).
    return false;
  }

  for (auto S : staticRegion.streams) {
    auto &dynS = S->getDynStream(dynRegion->seqNum);

    if (dynS.hasZeroTripCount()) {
      // Streams with 0 TripCount will not allocate the last element.
      continue;
    }
    if (!dynS.hasUnsteppedElem()) {
      // We don't have element for this used stream.
      DYN_S_DPRINTF(dynS.dynStreamId,
                    "[NotDispatchStreamEnd] No UnsteppedElem.\n");
      return false;
    }
    /**
     * For LoopEliminatedStream, we have to wait until it's:
     * 1. The second last element if we are using that value.
     * 2. The last element otherwise.
     */
    if (S->isLoopEliminated()) {
      // We already checked that we have UnsteppedElement.
      auto elem = dynS.getFirstUnsteppedElem();
      if (staticRegion.step.skipStepSecondLastElemStreams.count(S)) {
        if (!elem->isInnerSecondLastElem()) {
          S_ELEMENT_DPRINTF(elem, "[NotDispatchStreamEnd] Not LoopEliminated "
                                  "InnerSecondLastElem.\n");
          return false;
        }
      } else {
        if (!elem->isLastElement()) {
          S_ELEMENT_DPRINTF(
              elem, "[NotDispatchStreamEnd] Not LoopEliminated LastElem.\n");
          return false;
        }
      }
    }
  }

  return true;
}

void StreamRegionController::dispatchStreamEnd(const EndArgs &args) {
  const auto &streamRegion = this->se->getStreamRegion(args.infoRelativePath);
  const auto &endStreamInfos = streamRegion.streams();

  SE_DPRINTF("Dispatch StreamEnd for %s.\n", streamRegion.region());
  assert(this->canDispatchStreamEnd(args) &&
         "StreamEnd without unstepped elements.");

  /**
   * Dedup the coalesced stream ids.
   */
  std::unordered_set<Stream *> endedStreams;
  for (auto iter = endStreamInfos.rbegin(), end = endStreamInfos.rend();
       iter != end; ++iter) {
    // Release in reverse order.
    auto streamId = iter->id();
    auto S = this->se->getStream(streamId);
    if (endedStreams.count(S) != 0) {
      continue;
    }
    endedStreams.insert(S);

    // 1. Step one element.
    auto &dynS = S->getFirstAliveDynStream();
    if (!dynS.hasZeroTripCount()) {
      // Streams with 0 TripCount will not allocate the last element.
      dynS.stepElement(true /* isEnd */);
    }

    // 2. Mark the dynamicStream as ended.
    S->dispatchStreamEnd(args.seqNum);
  }
}

bool StreamRegionController::canExecuteStreamEnd(const EndArgs &args) {
  const auto &streamRegion = this->se->getStreamRegion(args.infoRelativePath);
  const auto &endStreamInfos = streamRegion.streams();

  SE_DPRINTF("CanExecute StreamEnd for %s.\n", streamRegion.region());
  /**
   * Dedup the coalesced stream ids.
   */
  std::unordered_set<Stream *> endedStreams;
  for (auto iter = endStreamInfos.rbegin(), end = endStreamInfos.rend();
       iter != end; ++iter) {
    // Release in reverse order.
    auto streamId = iter->id();
    auto S = this->se->getStream(streamId);
    if (endedStreams.count(S) != 0) {
      continue;
    }
    endedStreams.insert(S);
    // Check for StreamAck. So far that's only floating store stream.
    const auto &dynS = S->getDynStreamByEndSeqNum(args.seqNum);
    if (S->isStoreStream()) {
      if (!dynS.configExecuted || dynS.configSeqNum >= args.seqNum) {
        return false;
      }
      if (dynS.isFloatedToCache() &&
          dynS.cacheAckedElements.size() + dynS.stepElemCount <
              dynS.getNumFloatedElemUntil(dynS.FIFOIdx.entryIdx)) {
        // We are not ack the LastElement.
        DYN_S_DPRINTF(
            dynS.dynStreamId,
            "Cannot execute StreamEnd. Cache acked %llu, need %llu.\n",
            dynS.cacheAckedElements.size(), dynS.FIFOIdx.entryIdx);
        return false;
      }
    }
  }
  return true;
}

void StreamRegionController::rewindStreamEnd(const EndArgs &args) {
  const auto &streamRegion = this->se->getStreamRegion(args.infoRelativePath);
  const auto &endStreamInfos = streamRegion.streams();

  SE_DPRINTF("Rewind StreamEnd for %s.\n", streamRegion.region());

  /**
   * Dedup the coalesced stream ids.
   */
  std::unordered_set<Stream *> endedStreams;
  for (auto iter = endStreamInfos.rbegin(), end = endStreamInfos.rend();
       iter != end; ++iter) {
    // Rewind in reverse order.
    auto streamId = iter->id();
    auto S = this->se->getStream(streamId);
    if (endedStreams.count(S) != 0) {
      continue;
    }
    endedStreams.insert(S);

    // 1. Restart the last dynamic stream.
    S->rewindStreamEnd(args.seqNum);

    // 2. Unstep one element.
    auto &dynS = S->getFirstAliveDynStream();
    if (!dynS.hasZeroTripCount()) {
      dynS.unstepElement();
    }
  }
}

bool StreamRegionController::canCommitStreamEnd(const EndArgs &args) {
  const auto &streamRegion = this->se->getStreamRegion(args.infoRelativePath);
  const auto &endStreamInfos = streamRegion.streams();
  const auto &staticStreamRegion = this->getStaticRegion(streamRegion.region());

  for (auto iter = endStreamInfos.rbegin(), end = endStreamInfos.rend();
       iter != end; ++iter) {
    auto streamId = iter->id();
    auto S = this->se->getStream(streamId);
    const auto &dynS = S->getDynStreamByEndSeqNum(args.seqNum);

    if (dynS.hasZeroTripCount()) {
      // Streams with 0 TripCount does not have last element.
      continue;
    }
    auto endElement = dynS.tail->next;
    auto endElementIdx = endElement->FIFOIdx.entryIdx;

    /**
     * For eliminated loop, check for TotalTripCount.
     */
    if (S->isLoopEliminated() && dynS.hasTotalTripCount()) {
      uint64_t endElemOffset =
          staticStreamRegion.step.skipStepSecondLastElemStreams.count(S) ? 1
                                                                         : 0;
      if (endElementIdx + endElemOffset < dynS.getTotalTripCount()) {
        S_ELEMENT_DPRINTF(
            endElement,
            "[StreamEnd] Cannot commit as less TripCount %llu + %llu < %llu.\n",
            endElementIdx, endElemOffset, dynS.getTotalTripCount());
        return false;
      }
    }

    // There is always a dummy element for StreamEnd to step through.
    if (S->getEnabledStoreFunc()) {
      /**
       * We need to check that all stream element has acked in range-sync.
       * Normally this is enforced in canCommitStreamStep().
       * However, with range-sync, we have to commit StreamStep first to allow
       * remote streams commit.
       * Therefore, we wait here to check that we collected the last StreamAck.
       */
      bool shouldCheckAck = false;
      if (dynS.isFloatedToCache() && !dynS.shouldCoreSEIssue() &&
          dynS.shouldRangeSync() && endElementIdx > 0) {
        shouldCheckAck = true;
      }
      /**
       * Floated AtomicCompute/UpdateStream has to check Ack when:
       *                    w/ RangeSync       w/o. RangeSync
       * CoreIssue          Check              NoCheck
       * CoreNotIssue       Check              Check
       */
      if ((S->isAtomicComputeStream() || S->isUpdateStream()) &&
          dynS.isFloatedToCache() && endElementIdx > 0) {
        if (dynS.shouldRangeSync()) {
          shouldCheckAck = true;
        } else if (!dynS.shouldCoreSEIssue()) {
          shouldCheckAck = true;
        }
      }
      if (shouldCheckAck && dynS.cacheAckedElements.size() <
                                dynS.getNumFloatedElemUntil(endElementIdx)) {
        S_ELEMENT_DPRINTF(
            endElement,
            "[StreamEnd] Cannot commit as not enough Ack %llu < %llu.\n",
            dynS.cacheAckedElements.size(),
            dynS.getNumFloatedElemUntil(endElementIdx));
        return false;
      }
    }
    /**
     * Similarly to the above case, we also check that we collected the last
     * StreamDone.
     * TODO: These two cases should really be merged in the future.
     */
    if (dynS.isFloatedToCacheAsRoot() && dynS.shouldRangeSync()) {
      if (dynS.getNextCacheDoneElemIdx() < endElementIdx) {
        S_ELEMENT_DPRINTF(endElement,
                          "[StreamEnd] Cannot commit as no Done for %llu, "
                          "NextCacheDone %llu.\n",
                          endElementIdx, dynS.getNextCacheDoneElemIdx());
        return false;
      }
    }
    S_ELEMENT_DPRINTF(endElement,
                      "[StreamEnd] Can commit end element. FloatedToCache %d. "
                      "ShouldCoreSEIssue %d. Acked %d.\n",
                      dynS.isFloatedToCache(), dynS.shouldCoreSEIssue(),
                      dynS.cacheAckedElements.size());
  }
  return true;
}

void StreamRegionController::commitStreamEnd(const EndArgs &args) {

  const auto &streamRegion = this->se->getStreamRegion(args.infoRelativePath);
  auto &staticRegion = this->getStaticRegion(streamRegion.region());

  SE_DPRINTF("Commit StreamEnd for %s.\n", streamRegion.region());

  assert(!staticRegion.dynRegions.empty() && "Missing DynRegion.");

  const auto &dynRegion = staticRegion.dynRegions.front();
  if (dynRegion.seqNum > args.seqNum) {
    /**
     * We allow the == case because in nested stream, it is still
     * possible that InnerStreamEnd comes right after OuterStreamConfig,
     * leaving there no space to insert the InnerStreamConfig.
     */
    SE_PANIC("[Region] %s End (%lu) before Configure (%lu).\n",
             streamRegion.region(), args.seqNum, dynRegion.seqNum);
  }

  SE_DPRINTF(
      "[Region] Release DynRegion SeqNum %llu for region %s, remaining %llu.\n",
      dynRegion.seqNum, streamRegion.region(),
      staticRegion.dynRegions.size() - 1);
  this->checkRemainingNestRegions(dynRegion);

  this->activeDynRegionMap.erase(dynRegion.seqNum);
  staticRegion.dynRegions.pop_front();

  /**
   * Deduplicate the streams due to coalescing.
   * Releasing is again in two phases:
   * 1. Release all elements first.
   * 2. Release all dynamic streams.
   * This is to ensure that all dynamic streams are released at the same time.
   */
  const auto &endStreamInfos = streamRegion.streams();
  std::unordered_set<Stream *> endedStreams;
  for (auto iter = endStreamInfos.rbegin(), end = endStreamInfos.rend();
       iter != end; ++iter) {
    // Release in reverse order.
    auto streamId = iter->id();
    auto S = this->se->getStream(streamId);
    if (endedStreams.count(S) != 0) {
      continue;
    }
    endedStreams.insert(S);

    assert(!S->dynamicStreams.empty() &&
           "Failed to find ended DynamicInstanceState.");
    auto &endedDynS = S->dynamicStreams.front();
    if (endedDynS.hasZeroTripCount()) {
      // Streams with 0 TripCount does not have last element.
      continue;
    }
    /**
     * Release all unstepped element until there is none.
     */
    while (this->se->releaseElementUnstepped(endedDynS)) {
    }

    /**
     * Release the last element we stepped at dispatch.
     */
    this->se->releaseElementStepped(&endedDynS, true /* isEnd */,
                                    false /* doThrottle */);
  }
  std::vector<DynStream *> endedDynStreams;
  for (auto S : endedStreams) {
    assert(!S->dynamicStreams.empty() &&
           "Failed to find ended DynamicInstanceState.");
    auto &endedDynS = S->dynamicStreams.front();
    endedDynStreams.push_back(&endedDynS);

    /**
     * Sanity check that we allocated the correct total number of elements.
     */
    if (endedDynS.hasTotalTripCount()) {
      uint64_t endElemOffset =
          staticRegion.step.skipStepSecondLastElemStreams.count(S) ? 1 : 0;
      if (endedDynS.hasZeroTripCount()) {
        if (endedDynS.FIFOIdx.entryIdx + endElemOffset != 0) {
          DYN_S_PANIC(
              endedDynS.dynStreamId,
              "ZeroTripCount should never allocate. NextElemIdx %llu + %llu.\n",
              endedDynS.FIFOIdx.entryIdx, endElemOffset);
        }
      } else {
        if (endedDynS.getTotalTripCount() + endedDynS.stepElemCount !=
            endedDynS.FIFOIdx.entryIdx + endElemOffset) {
          DYN_S_PANIC(
              endedDynS.dynStreamId,
              "Commit End with TripCount %llu != NextElementIdx %llu + %llu.\n",
              endedDynS.getTotalTripCount(), endedDynS.FIFOIdx.entryIdx,
              endElemOffset);
        }
      }
    }
  }
  this->se->floatController->endFloatStreams(endedDynStreams);
  for (auto S : endedStreams) {
    // Notify the stream.
    S->commitStreamEnd(args.seqNum);
  }
}
