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

namespace gem5 {

bool StreamRegionController::canDispatchStreamEnd(const EndArgs &args) {
  const auto &streamRegion = this->se->getStreamRegion(args.infoRelativePath);
  auto &staticRegion = this->getStaticRegion(streamRegion.region());

  auto dynRegion = this->tryGetNextEndDynRegion(staticRegion);
  if (!dynRegion) {
    // It's possible that the Stream has not configured yet (e.g., Nest).
    return false;
  }
  return this->canDispatchStreamEndImpl(staticRegion, *dynRegion);
}

bool StreamRegionController::canDispatchStreamEndImpl(
    StaticRegion &staticRegion, DynRegion &dynRegion) {

  for (auto S : staticRegion.streams) {
    auto &dynS = S->getDynStream(dynRegion.seqNum);

    if (dynS.hasZeroTripCount()) {
      // Streams with 0 TripCount will not allocate the last element.
      continue;
    }
    if (S->isStoreComputeStream() && S->isLoopEliminated() &&
        !dynS.isFloatedToCache()) {
      // LoopElimStoreS may not allocate the last elem.
      // Especially if they are using the inner loop reduction result.
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
          S_ELEMENT_DPRINTF(elem,
                            "[NotDispatchStreamEnd] Not LoopEliminated "
                            "InnerSecondLastElem TripCount %ld.\n",
                            dynS.getTotalTripCount());
          return false;
        }
      } else {
        if (!elem->isLastElement()) {
          S_ELEMENT_DPRINTF(elem,
                            "[NotDispatchStreamEnd] Not LoopEliminated "
                            "LastElem TripCount %ld.\n",
                            dynS.getTotalTripCount());
          return false;
        }
      }
    }
  }

  return true;
}

void StreamRegionController::dispatchStreamEnd(const EndArgs &args) {

  const auto &streamRegion = this->se->getStreamRegion(args.infoRelativePath);

  SE_DPRINTF("Dispatch StreamEnd for %s.\n", streamRegion.region());
  assert(this->canDispatchStreamEnd(args) &&
         "StreamEnd without unstepped elements.");

  auto &staticRegion = this->getStaticRegion(streamRegion.region());
  auto &dynRegion = this->getNextEndDynRegion(staticRegion);

  this->dispatchStreamEndImpl(args, staticRegion, dynRegion);
}

void StreamRegionController::dispatchStreamEndImpl(const EndArgs &args,
                                                   StaticRegion &staticRegion,
                                                   DynRegion &dynRegion) {

  auto regionEndSeqNum = args.seqNum;
  if (this->se->myParams->enableO3ElimStreamEnd) {
    // Introduce one level of indirection between InstEndSeqNum <->
    // RegionEndSeqNum.
    regionEndSeqNum = dynRegion.seqNum + 1;
  }
  this->recordEndRegionSeqNum(args.seqNum, regionEndSeqNum);
  dynRegion.dispatchStreamEnd(regionEndSeqNum);

  for (auto S : staticRegion.streams) {
    auto &dynS = S->getDynStream(dynRegion.seqNum);

    /**
     * 1. Step one element.
     * Certain cases when we donot allocate the last elem, hence no stepping.
     * a. Stream with zero trip count.
     * b. LoopElimInCoreStoreCmpS.
     */
    if (dynS.hasZeroTripCount() || dynS.isLoopElimInCoreStoreCmpS()) {
      if (dynS.hasUnsteppedElem()) {
        S_ELEMENT_PANIC(dynS.getFirstUnsteppedElem(),
                        "Should not allocate last elem.");
      }
    } else {
      dynS.stepElement(true /* isEnd */);
    }

    // 2. Mark the dynamicStream as ended.
    dynS.dispatchStreamEnd(regionEndSeqNum);
  }
}

bool StreamRegionController::canExecuteStreamEnd(const EndArgs &args) {
  const auto &streamRegion = this->se->getStreamRegion(args.infoRelativePath);

  SE_DPRINTF("CanExecute StreamEnd for %s.\n", streamRegion.region());

  auto &staticRegion = this->getStaticRegion(streamRegion.region());
  auto &dynRegion = this->getDynRegionByEndSeqNum(staticRegion, args.seqNum);

  return this->canExecuteStreamEndImpl(staticRegion, dynRegion);
}

bool StreamRegionController::canExecuteStreamEndImpl(StaticRegion &staticRegion,
                                                     DynRegion &dynRegion) {
  for (auto S : staticRegion.streams) {
    auto &dynS = S->getDynStream(dynRegion.seqNum);
    if (S->isStoreStream()) {
      if (!dynS.configExecuted) {
        return false;
      }
      if (dynS.isFloatedToCache() &&
          dynS.cacheAckedElements.size() + dynS.stepElemCount <
              dynS.getNumFloatedElemUntil(dynS.FIFOIdx.entryIdx)) {
        // We are not ack the LastElement.
        DYN_S_DPRINTF(dynS.dynStreamId,
                      "[NotExecuteStreamEnd] CacheAcked %llu + StepElemCount "
                      "%ld < Floated %llu.\n",
                      dynS.cacheAckedElements.size(), dynS.stepElemCount,
                      dynS.getNumFloatedElemUntil(dynS.FIFOIdx.entryIdx));
        return false;
      }
    }
  }
  return true;
}

void StreamRegionController::rewindStreamEnd(const EndArgs &args) {
  const auto &streamRegion = this->se->getStreamRegion(args.infoRelativePath);

  auto &staticRegion = this->getStaticRegion(streamRegion.region());
  auto &dynRegion = this->getDynRegionByEndSeqNum(staticRegion, args.seqNum);

  this->eraseEndRegionSeqNum(args.seqNum);
  dynRegion.rewindStreamEnd();

  SE_DPRINTF("Rewind StreamEnd for %s.\n", streamRegion.region());

  for (auto S : staticRegion.streams) {
    auto &dynS = S->getDynStream(dynRegion.seqNum);

    // 1. Restart the last dynamic stream.
    dynS.rewindStreamEnd();

    // 2. Unstep one element.
    if (!dynS.hasZeroTripCount()) {
      dynS.unstepElement();
    }
  }
}

bool StreamRegionController::canCommitStreamEnd(const EndArgs &args) {
  const auto &streamRegion = this->se->getStreamRegion(args.infoRelativePath);
  auto &staticRegion = this->getStaticRegion(streamRegion.region());

  auto &dynRegion = this->getDynRegionByEndSeqNum(staticRegion, args.seqNum);

  return this->canCommitStreamEndImpl(staticRegion, dynRegion);
}

bool StreamRegionController::canCommitStreamEndImpl(StaticRegion &staticRegion,
                                                    DynRegion &dynRegion) {

  // Can not release region if we have nest regions.
  if (this->hasRemainingNestRegions(dynRegion)) {
    SE_DPRINTF("[StreamEnd] NoCommit as RemainNestRegions.\n");
    return false;
  }
  for (auto S : staticRegion.streams) {
    auto &dynS = S->getDynStream(dynRegion.seqNum);

    if (dynS.hasZeroTripCount() || dynS.isLoopElimInCoreStoreCmpS()) {
      // Some streams do not have last element.
      continue;
    }
    auto endElem = dynS.tail->next;
    auto endElemIdx = endElem->FIFOIdx.entryIdx;

    /**
     * For eliminated loop, check for TotalTripCount.
     */
    if (S->isLoopEliminated() && dynS.hasTotalTripCount()) {
      uint64_t endElemOffset =
          staticRegion.step.skipStepSecondLastElemStreams.count(S) ? 1 : 0;
      if (endElemIdx + endElemOffset < dynS.getTotalTripCount()) {
        S_ELEMENT_DPRINTF(
            endElem,
            "[StreamEnd] NoCommit as less TripCount %llu + %llu < %llu.\n",
            endElemIdx, endElemOffset, dynS.getTotalTripCount());
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
          dynS.shouldRangeSync() && endElemIdx > 0) {
        shouldCheckAck = true;
      }
      /**
       * Floated AtomicCompute/UpdateStream has to check Ack when:
       *                    w/ RangeSync       w/o. RangeSync
       * CoreIssue          Check              NoCheck
       * CoreNotIssue       Check              Check
       */
      if ((S->isAtomicComputeStream() || S->isUpdateStream()) &&
          dynS.isFloatedToCache() && endElemIdx > 0) {
        if (dynS.shouldRangeSync()) {
          shouldCheckAck = true;
        } else if (!dynS.shouldCoreSEIssue()) {
          shouldCheckAck = true;
        }
      }
      if (shouldCheckAck && dynS.cacheAckedElements.size() <
                                dynS.getNumFloatedElemUntil(endElemIdx)) {
        S_ELEMENT_DPRINTF(
            endElem,
            "[StreamEnd] Cannot commit as not enough Ack %llu < %llu.\n",
            dynS.cacheAckedElements.size(),
            dynS.getNumFloatedElemUntil(endElemIdx));
        return false;
      }
    }
    /**
     * Similarly to the above case, we also check that we collected the last
     * StreamDone.
     * TODO: These two cases should really be merged in the future.
     */
    if (dynS.isFloatedToCacheAsRoot() && dynS.shouldRangeSync()) {
      if (dynS.getNextCacheDoneElemIdx() < endElemIdx) {
        S_ELEMENT_DPRINTF(endElem,
                          "[StreamEnd] NoCommit as no Done for %llu, "
                          "NextCacheDone %llu.\n",
                          endElemIdx, dynS.getNextCacheDoneElemIdx());
        return false;
      }
    }
    S_ELEMENT_DPRINTF(endElem,
                      "[StreamEnd] Can commit end elem. FloatedToCache %d. "
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

  this->se->numInflyStreamConfigurations--;
  assert(this->se->numInflyStreamConfigurations >= 0 &&
         "Negative infly StreamConfigurations.");

  auto &dynRegion = this->getDynRegionByEndSeqNum(staticRegion, args.seqNum);
  if (dynRegion.seqNum > dynRegion.endSeqNum) {
    /**
     * We allow the == case because in nested stream, it is still
     * possible that InnerStreamEnd comes right after OuterStreamConfig,
     * leaving there no space to insert the InnerStreamConfig.
     */
    SE_PANIC("[Region] %s End (%lu) before Configure (%lu).\n",
             streamRegion.region(), dynRegion.endSeqNum, dynRegion.seqNum);
  }

  SE_DPRINTF(
      "[Region] Release DynRegion SeqNum %llu %s Remaining %llu Total %llu.\n",
      dynRegion.seqNum, streamRegion.region(),
      staticRegion.dynRegions.size() - 1, this->activeDynRegionMap.size() - 1);
  if (this->hasRemainingNestRegions(dynRegion)) {
    SE_PANIC("[Region] %s End with Remaining NestRegions.",
             streamRegion.region());
  }

  /**
   * Deduplicate the streams due to coalescing.
   * Releasing is again in two phases:
   * 1. Release all elements first.
   * 2. Release all dynamic streams.
   * This is to ensure that all dynamic streams are released at the same time.
   */
  for (auto S : staticRegion.streams) {
    auto &dynS = S->getDynStream(dynRegion.seqNum);

    // Remove from the IssueList.
    this->se->removeIssuingDynS(&dynS);

    // Release in reverse order.
    if (dynS.hasZeroTripCount() || dynS.isLoopElimInCoreStoreCmpS()) {
      // Some streams do not have last element.
      continue;
    }
    /**
     * Release all unstepped element until there is none.
     */
    while (this->se->releaseElementUnstepped(dynS)) {
    }

    /**
     * Release the last element we stepped at dispatch.
     */
    this->se->releaseElementStepped(&dynS, true /* isEnd */,
                                    false /* doThrottle */);
  }
  std::vector<DynStream *> endedDynStreams;
  for (auto S : staticRegion.streams) {
    auto &dynS = S->getDynStream(dynRegion.seqNum);
    endedDynStreams.push_back(&dynS);

    /**
     * Sanity check that we allocated the correct total number of elements.
     */
    if (dynS.hasTotalTripCount()) {
      uint64_t endElemOffset =
          staticRegion.step.skipStepSecondLastElemStreams.count(S) ? 1 : 0;
      if (dynS.hasZeroTripCount() || dynS.isLoopElimInCoreStoreCmpS()) {
        if (dynS.FIFOIdx.entryIdx + endElemOffset != dynS.getTotalTripCount()) {
          DYN_S_PANIC(
              dynS.dynStreamId,
              "Commit End with TripCount %llu != NextElemIdx %llu + %llu.\n",
              dynS.getTotalTripCount(), dynS.FIFOIdx.entryIdx, endElemOffset);
        }
      } else {
        if (dynS.getTotalTripCount() + dynS.stepElemCount !=
            dynS.FIFOIdx.entryIdx + endElemOffset) {
          DYN_S_PANIC(dynS.dynStreamId,
                      "Commit End with TripCount %llu + %ld != NextElemIdx "
                      "%llu + %llu.\n",
                      dynS.getTotalTripCount(), dynS.stepElemCount,
                      dynS.FIFOIdx.entryIdx, endElemOffset);
        }
      }
    }
  }
  this->se->floatController->endFloatStreams(endedDynStreams);
  for (auto S : staticRegion.streams) {
    // Notify the stream.
    auto &dynS = S->getDynStream(dynRegion.seqNum);
    dynS.commitStreamEnd();
    S->releaseDynStream(dynS.configSeqNum);
  }

  this->activeDynRegionMap.erase(dynRegion.seqNum);
  bool erasedDynRegion = false;
  for (auto iter = staticRegion.dynRegions.begin();
       iter != staticRegion.dynRegions.end(); ++iter) {
    if (iter->seqNum == dynRegion.seqNum) {
      erasedDynRegion = true;

      // Notify the NestParentSE about the release.
      if (dynRegion.nestParentSE) {
        auto nestParentDynConfig = dynRegion.nestParentDynConfig;
        auto erasedFromNestParent = false;
        for (auto iter = nestParentDynConfig->nestDynRegions.begin(),
                  end = nestParentDynConfig->nestDynRegions.end();
             iter != end; ++iter) {
          if (iter->configSeqNum == dynRegion.seqNum) {
            assert(iter->configSE == this->se);
            erasedFromNestParent = true;
            nestParentDynConfig->nestDynRegions.erase(iter);
            break;
          }
        }
        assert(erasedFromNestParent);
      }

      staticRegion.dynRegions.erase(iter);
      break;
    }
  }
  assert(erasedDynRegion && "Failed to erase DynRegion.");
}

void StreamRegionController::recordEndRegionSeqNum(uint64_t instEndSeqNum,
                                                   uint64_t regionEndSeqNum) {
  assert(this->instToRegionEndSeqNumMap.emplace(instEndSeqNum, regionEndSeqNum)
             .second &&
         "Already Inserted InstEndSeqNum.");
}

void StreamRegionController::eraseEndRegionSeqNum(uint64_t instEndSeqNum) {
  assert(this->instToRegionEndSeqNumMap.count(instEndSeqNum) &&
         "Missing InstEndSeqNum");
  this->instToRegionEndSeqNumMap.erase(instEndSeqNum);
}

StreamRegionController::DynRegion *
StreamRegionController::tryGetFirstAliveDynRegion(StaticRegion &staticRegion) {
  for (auto &dynRegion : staticRegion.dynRegions) {
    if (!dynRegion.endDispatched) {
      return &dynRegion;
    }
  }
  return nullptr;
}

StreamRegionController::DynRegion &
StreamRegionController::getFirstAliveDynRegion(StaticRegion &staticRegion) {
  for (auto &dynRegion : staticRegion.dynRegions) {
    if (!dynRegion.endDispatched) {
      return dynRegion;
    }
  }
  SE_PANIC("No Alive DynRegion.");
}

StreamRegionController::DynRegion *
StreamRegionController::tryGetNextEndDynRegion(StaticRegion &staticRegion) {
  if (!this->se->myParams->enableO3ElimStreamEnd) {
    // Default in-order StreamEnd.
    return this->tryGetFirstAliveDynRegion(staticRegion);
  }
  /**
   * For out-of-order StreamEnd, we try to find one that can
   * dispatch/execute/commit.
   */
  for (auto &dynRegion : staticRegion.dynRegions) {
    if (dynRegion.endDispatched) {
      continue;
    }
    if (!this->canDispatchStreamEndImpl(staticRegion, dynRegion) ||
        !this->canExecuteStreamEndImpl(staticRegion, dynRegion) ||
        !this->canCommitStreamEndImpl(staticRegion, dynRegion)) {
      continue;
    }
    return &dynRegion;
  }
  return nullptr;
}

StreamRegionController::DynRegion &
StreamRegionController::getNextEndDynRegion(StaticRegion &staticRegion) {
  if (auto dynRegion = this->tryGetNextEndDynRegion(staticRegion)) {
    return *dynRegion;
  }
  SE_PANIC("No EndDynRegion.");
}

StreamRegionController::DynRegion &
StreamRegionController::getDynRegionByEndSeqNum(StaticRegion &staticRegion,
                                                uint64_t instEndSeqNum) {
  auto regionEndSeqNum = this->instToRegionEndSeqNumMap.at(instEndSeqNum);
  for (auto &dynRegion : staticRegion.dynRegions) {
    if (dynRegion.endDispatched && dynRegion.endSeqNum == regionEndSeqNum) {
      return dynRegion;
    }
  }
  SE_PANIC("No Ended DynRegion.");
}
} // namespace gem5
