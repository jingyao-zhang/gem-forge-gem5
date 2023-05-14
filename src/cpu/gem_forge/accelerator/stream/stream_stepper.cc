#include "stream_region_controller.hh"

#include "base/trace.hh"
#include "debug/StreamLoopBound.hh"

#define DEBUG_TYPE StreamLoopBound
#include "stream_log.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamLoopBound, format, ##args)
#define SE_PANIC(format, args...)                                              \
  panic("[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)

namespace gem5 {

void StreamRegionController::initializeStep(
    const ::LLVM::TDG::StreamRegion &region, StaticRegion &staticRegion) {

  // Just remember the StepRootStreams.
  SE_DPRINTF("[Stepper] Initialized StaticStep for region %s.\n",
             region.region());
  auto &staticStep = staticRegion.step;
  for (auto S : staticRegion.streams) {
    if (S->stepRootStream == S) {
      SE_DPRINTF(
          "[Stepper] Add StepRootStream InnerLoop %d ConfigLoop %d %s.\n",
          S->getLoopLevel(), S->getConfigLoopLevel(), S->getStreamName());
      staticStep.stepGroups.emplace_back(S);
      staticStep.stepRootStreams.push_back(S);
    }
  }
  // Sort from inner-most to outer-most loop level.
  std::sort(staticStep.stepGroups.begin(), staticStep.stepGroups.end(),
            [](const StaticRegion::StaticStep::StepGroupInfo &A,
               const StaticRegion::StaticStep::StepGroupInfo &B) -> bool {
              return A.stepRootS->getLoopLevel() > B.stepRootS->getLoopLevel();
            });
  for (auto S : staticRegion.streams) {
    panic_if(!S->stepRootStream, "Missing StepRootS %s.", S->getStreamName());
    StaticRegion::StaticStep::StepGroupInfo *stepGroup = nullptr;
    for (auto &group : staticStep.stepGroups) {
      if (group.stepRootS == S->stepRootStream) {
        stepGroup = &group;
        break;
      }
    }
    panic_if(!stepGroup, "Missing StepGroup %s.", S->getStreamName());
    if (S->isInnerFinalValueUsedByCore()) {
      SE_DPRINTF("[Stepper] NeedFinalValue %s.\n", S->getStreamName());
      stepGroup->needFinalValue = true;
    }
    if (S->isInnerSecondFinalValueUsedByCore()) {
      SE_DPRINTF("[Stepper] NeedSecondFinalValue %s.\n", S->getStreamName());
      stepGroup->needSecondFinalValue = true;
      staticStep.skipStepSecondLastElemStreams.insert(S->stepRootStream);
      staticStep.skipStepSecondLastElemStreams.insert(S);
    }
    if (stepGroup->needSecondFinalValue && stepGroup->needFinalValue) {
      SE_PANIC("[Stepper] Need both FinalValue and SecondFinalValue %s.",
               stepGroup->stepRootS->getStreamName());
    }
  }
  // Populate for all streams whether we skip step the second last elem.
  for (auto S : staticRegion.streams) {
    panic_if(!S->stepRootStream, "Missing StepRootS %s.", S->getStreamName());
    if (staticStep.skipStepSecondLastElemStreams.count(S->stepRootStream)) {
      SE_DPRINTF("[Stepper] SkipStepSecondLastElem %s.\n", S->getStreamName());
      staticStep.skipStepSecondLastElemStreams.insert(S);
    }
  }

  if (staticStep.stepGroups.empty()) {
    SE_PANIC("[Stepper] No StepRootStream for %s.", region.region());
  }
}

void StreamRegionController::dispatchStreamConfigForStep(const ConfigArgs &args,
                                                         DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  SE_DPRINTF("[Stepper] Initialized DynStep for %s.\n",
             staticRegion.region.region());
  for (auto staticGroupIdx = 0;
       staticGroupIdx < staticRegion.step.stepGroups.size(); ++staticGroupIdx) {
    const auto &staticGroup = staticRegion.step.stepGroups[staticGroupIdx];
    dynRegion.step.stepGroups.emplace_back(
        staticGroup.stepRootS->getLoopLevel(), staticGroupIdx);
  }
}

void StreamRegionController::executeStreamConfigForStep(const ConfigArgs &args,
                                                        DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  if (!staticRegion.region.is_loop_bound()) {
    /**
     * Without StreamLoopBound, we check for TotalTripCount.
     *
     * An interesting case is some StepGroup may have Zero TripCount.
     * This is possible when the compiler can not perfectly vectorize a loop and
     * leaves some Loop Epilogue. At run time, if the parameters are right, we
     * may not execute the epilogue loop (with TripCount zero).
     *
     * In such case we remove the Group and never step them
     */
    const auto &staticGroups = staticRegion.step.stepGroups;
    auto &dynGroups = dynRegion.step.stepGroups;
    // First collect trip counts.
    for (auto &dynGroup : dynGroups) {
      auto S = staticGroups[dynGroup.staticGroupIdx].stepRootS;
      auto &dynS = S->getDynStream(dynRegion.seqNum);
      if (!dynS.hasTotalTripCount()) {
        if (staticRegion.someStreamsLoopEliminated) {
          DYN_S_PANIC(
              dynS.dynStreamId,
              "[Stepper] EliminatedLoop w/o. LoopBound must has TripCount.");
        }
      }
      dynGroup.totalTripCount = dynS.getTotalTripCount();
      dynGroup.levelTripCount = dynGroup.totalTripCount;
      DYN_S_DPRINTF(dynS.dynStreamId, "[Stepper] Get TripCount %ld.\n",
                    dynGroup.totalTripCount);
    }

    // Remove DynGroup with 0 trip count.
    for (auto iter = dynGroups.begin(); iter != dynGroups.end();) {
      if (iter->totalTripCount > 0) {
        ++iter;
        continue;
      } else {
        auto S = staticGroups[iter->staticGroupIdx].stepRootS;
        auto &dynS = S->getDynStream(dynRegion.seqNum);
        DYN_S_DPRINTF(dynS.dynStreamId,
                      "[Stepper] Removed due to Zero TripCount.\n");
        iter = dynGroups.erase(iter);
      }
    }

    // Compute and sanity check for LevelTripCount.
    for (int groupIdx = 1; groupIdx < dynGroups.size(); ++groupIdx) {
      auto &dynGroup = dynGroups[groupIdx];
      auto S = staticGroups[dynGroup.staticGroupIdx].stepRootS;
      auto &dynS = S->getDynStream(dynRegion.seqNum);
      auto &prevDynGroup = dynGroups[groupIdx - 1];
      if (prevDynGroup.loopLevel == dynGroup.loopLevel) {
        if (prevDynGroup.totalTripCount != dynGroup.totalTripCount) {
          /**
           * Check that Groups with the same LoopLevel has the same TripCount.
           * There are possible exceptions when we have multiple sibling loops
           * with different trip count.
           * We don't support such cases, but when we are skipping to the
           * StreamEnd, we can ignore it.
           */
          if (!this->canSkipToStreamEnd(dynRegion) &&
              staticRegion.someStreamsLoopEliminated) {
            DYN_S_PANIC(dynS.dynStreamId,
                        "[Stepper] Mismatch TripCount in same LoopLevel %lld "
                        "!= %lld %s.",
                        prevDynGroup.totalTripCount, dynGroup.totalTripCount,
                        staticGroups[prevDynGroup.staticGroupIdx]
                            .stepRootS->getDynStream(dynRegion.seqNum)
                            .dynStreamId);
          }
        }
      } else {
        // Generate LevelTripCount for previous level.
        if (prevDynGroup.totalTripCount < dynGroup.totalTripCount) {
          DYN_S_PANIC(
              dynS.dynStreamId,
              "[Stepper] PrevGroup Loop %d Trip %ld <= Group %d Trip %ld.",
              prevDynGroup.loopLevel, prevDynGroup.totalTripCount,
              dynGroup.loopLevel, dynGroup.totalTripCount);
        }

        if (prevDynGroup.totalTripCount == DynStream::InvalidTripCount ||
            dynGroup.totalTripCount == DynStream::InvalidTripCount) {
          DYN_S_DPRINTF(dynS.dynStreamId,
                        "[Stepper] Cannot Set LevelTripCount as Invalid "
                        "PrevGroup Loop %d Trip %ld, Group %d Trip %ld.\n",
                        prevDynGroup.loopLevel, prevDynGroup.totalTripCount,
                        dynGroup.loopLevel, dynGroup.totalTripCount);
          continue;
        }

        assert(prevDynGroup.totalTripCount % dynGroup.totalTripCount == 0);
        auto levelTripCount =
            prevDynGroup.totalTripCount / dynGroup.totalTripCount;
        prevDynGroup.levelTripCount = levelTripCount;
        for (int i = groupIdx - 2; i >= 0; --i) {
          if (dynGroups[i].loopLevel != prevDynGroup.loopLevel) {
            break;
          }
          dynGroups[i].levelTripCount =
              dynGroups[i].totalTripCount / dynGroup.totalTripCount;
        }
      }
    }

    // Print the LevelTripCount and set.
    for (int groupIdx = 0; groupIdx < dynGroups.size(); ++groupIdx) {
      auto &dynGroup = dynGroups[groupIdx];
      auto S = staticGroups[dynGroup.staticGroupIdx].stepRootS;
      auto &dynS = S->getDynStream(dynRegion.seqNum);
      DYN_S_DPRINTF(
          dynS.dynStreamId,
          "[Stepper] LoopLevel %d TotalTripCount %lld LevelTripCount %lld.\n",
          dynGroup.loopLevel, dynGroup.totalTripCount, dynGroup.levelTripCount);
      if (dynGroup.totalTripCount != dynGroup.levelTripCount) {
        // We have a valid LevelTripCount.
        assert(dynGroup.levelTripCount != DynStream::InvalidTripCount);
        for (auto stepS : this->se->getStepStreamList(S)) {
          auto &stepDynS = stepS->getDynStream(dynRegion.seqNum);
          stepDynS.setInnerTripCount(dynGroup.levelTripCount);
        }
      }
    }

  } else {
    // LoopBound only works for single-level loops.
    for (auto S : staticRegion.streams) {
      if (S->getConfigLoopLevel() != S->getLoopLevel()) {
        S_PANIC(S, "[Stepper] Multi-Level with StreamLoopBound?");
      }
    }
  }
}

void StreamRegionController::commitStreamConfigForStep(const ConfigArgs &args,
                                                       DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  /**
   * Try to record FirstFloatElemIdx.
   */
  const auto &staticGroups = staticRegion.step.stepGroups;
  auto &dynGroups = dynRegion.step.stepGroups;
  // First collect trip counts.
  for (auto &dynGroup : dynGroups) {
    auto stepRootS = staticGroups[dynGroup.staticGroupIdx].stepRootS;
    for (auto S : this->se->getStepStreamList(stepRootS)) {
      const auto &dynS = S->getDynStream(dynRegion.seqNum);
      if (dynS.isFloatedToCache()) {
        auto firstFloatElemIdx = dynS.getFirstFloatElemIdx();
        if (dynGroup.firstFloatElemIdx ==
            DynRegion::DynStep::DynStepGroupInfo::InvalidFirstFloatElemIdx) {
          dynGroup.firstFloatElemIdx = firstFloatElemIdx;
          DYN_S_DPRINTF(dynS.dynStreamId,
                        "[Stepper] Set FirstFloatElemIdx %lu.\n",
                        firstFloatElemIdx);
        } else {
          if (dynGroup.firstFloatElemIdx != firstFloatElemIdx) {
            DYN_S_PANIC(dynS.dynStreamId,
                        "[Stepper] Mismatch in FirstFloatElemIdx %lu != %lu.",
                        firstFloatElemIdx, dynGroup.firstFloatElemIdx);
          }
        }
      }
    }
  }
}

void StreamRegionController::tryStepToStreamEnd(
    DynRegion &dynRegion, DynRegion::DynStep &dynStep,
    DynRegion::DynStep::DynStepGroupInfo &dynGroup, DynStream &stepRootDynS) {
  /**
   * If the DynRegion is skipped to the end, we need to adjust the NextElemIdx
   * once we have reached the AllocUntilElemIdx (see stream_allocator.cc).
   * 1. If not MidwayFloat (FirstFloatElemIdx == 0), skip when NextElemIdx == 0.
   * 2. If MidwayFloat (FirstFloatElemIdx > 0), skip when NextElemIdx ==
   * FirstFloatElemIdx + 1.
   */
  auto totalTripCount = stepRootDynS.getTotalTripCount();
  const auto firstFloatElemIdx = dynGroup.firstFloatElemIdx;
  if (firstFloatElemIdx ==
      DynRegion::DynStep::DynStepGroupInfo::InvalidFirstFloatElemIdx) {
    DYN_S_PANIC(stepRootDynS.dynStreamId,
                "[Stepper] Try StepToEnd without FirstFloatElemIdx.");
  }
  const auto allocUntilElemIdx =
      firstFloatElemIdx == 0 ? 0 : firstFloatElemIdx + 1;
  if (dynGroup.nextElemIdx == allocUntilElemIdx) {
    DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                  "[Stepper] SkipToEnd: Next %lu = Total %lu FirstFloat %lu "
                  "AllocUntil %lu.\n",
                  dynGroup.nextElemIdx, totalTripCount, firstFloatElemIdx,
                  allocUntilElemIdx);
    dynGroup.nextElemIdx = totalTripCount;
  }
}

void StreamRegionController::stepStream(DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  if (!staticRegion.someStreamsLoopEliminated) {
    return;
  }

  auto &dynStep = dynRegion.step;
  auto &dynGroup = dynStep.stepGroups[dynStep.nextDynGroupIdx];

  if (dynGroup.skipStep) {
    return;
  }

  auto &staticStep = staticRegion.step;
  auto &staticGroup = staticStep.stepGroups[dynGroup.staticGroupIdx];
  auto stepRootS = staticGroup.stepRootS;
  auto stepRootStreamId = stepRootS->staticId;
  auto &stepRootDynS = stepRootS->getDynStream(dynRegion.seqNum);

  if (dynRegion.canSkipToEnd && stepRootDynS.hasTotalTripCount()) {
    auto totalTripCount = stepRootDynS.getTotalTripCount();
    if (dynGroup.nextElemIdx < totalTripCount) {
      /**
       * Do not bother to skip until config committed. This is because
       * we need the FirstFloatElemIdx, which is only set when committing
       * the config.
       */
      if (!dynRegion.configCommitted) {
        DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                      "[Stepper] Wait For Config Commit to Skip.\n");
        return;
      }
      this->tryStepToStreamEnd(dynRegion, dynStep, dynGroup, stepRootDynS);
    }
  }

  /**
   * First check that we have handled possible NestStream and LoopBound
   * for the next stepped iteration.
   */
  const auto &dynBound = dynRegion.loopBound;
  if (staticRegion.region.is_loop_bound()) {
    if (dynGroup.nextElemIdx >= dynBound.nextElemIdx) {
      DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                    "[Stepper] Wait For LoopBound: %llu >= %llu.\n",
                    dynGroup.nextElemIdx, dynBound.nextElemIdx);
      return;
    }
  }
  // Check for TotalTripCount.
  if (stepRootDynS.hasTotalTripCount() &&
      dynGroup.nextElemIdx >= stepRootDynS.getTotalTripCount()) {
    DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                  "[Stepper] Wait For TripCount: %llu >= %llu. Set SkipStep.\n",
                  dynGroup.nextElemIdx, dynGroup.totalTripCount);
    dynGroup.skipStep = true;
    return;
  }

  for (const auto &dynNestConfig : dynRegion.nestConfigs) {
    if (dynGroup.nextElemIdx >= dynNestConfig.nextConfigElemIdx) {
      DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                    "[Stepper] Wait for NestRegion: %llu >= %llu Region %s.\n",
                    dynGroup.nextElemIdx, dynNestConfig.nextConfigElemIdx,
                    dynNestConfig.staticRegion->region.region());
      return;
    }
  }

  /**
   * Actually start to step.
   */
  if (dynGroup.nextElemIdx >= stepRootDynS.getTotalTripCount()) {
    DYN_S_PANIC(stepRootDynS.dynStreamId,
                "[Stepper] Step Beyond TripCount %lld >= %lld.",
                dynGroup.nextElemIdx, stepRootDynS.getTotalTripCount());
  }

  DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                "[Stepper] Try to Step RootDynS NextElem %llu.\n",
                dynGroup.nextElemIdx);

  auto stepToNextGroup = [&stepRootDynS, &dynStep, &dynGroup]() -> void {
    /**
     * Check for next group:
     * 1. If no next group -> round.
     * 2. Otherwise:
     *   a. If same loop level -> advance.
     *   b. If different loop level
     *        If reached LevelTripCount -> advance.
     *        Otherwise -> round.
     */
    dynGroup.nextElemIdx += dynGroup.stepElemCount;
    auto nextGroupIdx = dynStep.nextDynGroupIdx + 1;
    if (nextGroupIdx == dynStep.stepGroups.size()) {
      nextGroupIdx = 0;
    } else {
      auto &nextGroup = dynStep.stepGroups[nextGroupIdx];
      if (nextGroup.loopLevel != dynGroup.loopLevel) {
        if (dynGroup.nextElemIdx % dynGroup.levelTripCount != 0) {
          nextGroupIdx = 0;
        }
      }
    }
    DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                  "[Stepper] CurrentGroup %d Elem %llu -> %llu NextGroup %d.\n",
                  dynStep.nextDynGroupIdx, dynGroup.nextElemIdx - 1,
                  dynGroup.nextElemIdx, nextGroupIdx);
    dynStep.state = DynRegion::DynStep::StepState::BEFORE_DISPATCH;
    dynStep.nextDynGroupIdx = nextGroupIdx;
  };

  StreamEngine::StreamStepArgs args(stepRootStreamId);
  args.dynInstanceId = stepRootDynS.dynStreamId.streamInstance;

  switch (dynStep.state) {
  default: {
    SE_PANIC("[Stepper] Invalid State %d.", dynStep.state);
  }
  case DynRegion::DynStep::StepState::BEFORE_DISPATCH: {

    /**
     * If streams within this StepGroup needs to return the SecondLast value to
     * the core, we have to not step the SecondLast element. Therefore, here we
     * just directly advance to the next StepGroup.
     */
    if (staticGroup.needSecondFinalValue && stepRootDynS.hasTotalTripCount() &&
        dynGroup.nextElemIdx + 1 >= stepRootDynS.getTotalTripCount()) {
      DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                    "[Stepper] Skip Step SecondLast Element %lu.\n",
                    dynGroup.nextElemIdx);
      stepToNextGroup();
      break;
    }

    /**
     * There are actually two cases:
     * 1. The stream is LoopEliminated -- it is our job to step it.
     *   We will check StreamEngine's API.
     * 2. The stream is not LoopEliminated -- the core will step it.
     *   We will check the core's progress.
     * The second case is only our concern when loops are partially
     * eliminated, i.e., some inner loops are eliminated, while outer
     * loops are not.
     */

    if (stepRootS->isLoopEliminated()) {
      // Loop elimianted. Our job.
      if (se->canDispatchStreamStep(args)) {

        DYN_S_DPRINTF(stepRootDynS.dynStreamId, "[Stepper] Dispatch.\n");
        se->dispatchStreamStep(args);
        dynStep.state = DynRegion::DynStep::StepState::BEFORE_COMMIT;

      } else {
        DYN_S_DPRINTF(stepRootDynS.dynStreamId, "[Stepper] CanNot Dispatch.\n");
      }
    } else {
      // Loop not eliminated. Check the stream's progress.
      if (stepRootDynS.isElemStepped(dynGroup.nextElemIdx)) {

        DYN_S_DPRINTF(stepRootDynS.dynStreamId, "[Stepper] Core Dispatched.\n");
        dynStep.state = DynRegion::DynStep::StepState::BEFORE_COMMIT;

      } else {
        DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                      "[Stepper] Wait Core Dispatch.\n");
      }
    }
    break;
  }
  case DynRegion::DynStep::StepState::BEFORE_COMMIT: {

    if (stepRootS->isLoopEliminated()) {
      // Loop elimianted. Our job.
      if (se->canCommitStreamStep(args)) {

        DYN_S_DPRINTF(stepRootDynS.dynStreamId, "[Stepper] Commit.\n");
        se->commitStreamStep(args);
        stepToNextGroup();

      } else {
        DYN_S_DPRINTF(stepRootDynS.dynStreamId, "[Stepper] CanNot Commit.\n");
      }
      break;
    } else {
      // Loop not eliminated. Check the stream's progress.
      if (stepRootDynS.isElemReleased(dynGroup.nextElemIdx)) {

        DYN_S_DPRINTF(stepRootDynS.dynStreamId, "[Stepper] Core Committed.\n");
        stepToNextGroup();

      } else {
        DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                      "[Stepper] Wait Core Commit.\n");
      }
    }
  }
  }
}

bool StreamRegionController::endStream(DynRegion &dynRegion) {

  auto &staticRegion = *dynRegion.staticRegion;

  if (dynRegion.endCannotDispatch || dynRegion.endCannotCommit) {
    return false;
  }

  assert(staticRegion.shouldEndStream());

  if (dynRegion.endDispatched) {
    // We already ended this region.
    SE_PANIC("[Stepper] SE Ended Region %s.", staticRegion.region.region());
  }

  /**
   * Can we directly try to dispatch/execute/commit StreamEnd here?
   */
  auto configSeqNum = dynRegion.seqNum;
  auto endSeqNum = configSeqNum + 1;

  StreamEngine::StreamEndArgs args(endSeqNum,
                                   staticRegion.region.relative_path());

  SE_DPRINTF("[Stepper] Try End %s.\n", staticRegion.region.region());

  /**
   * We need some additional check for SE ended region:
   * 1. If has loop bound, it is evaluated.
   * 2. If the last element has a core user, wait for it. Usually happens for
   * reduction.
   */
  if (staticRegion.region.is_loop_bound()) {
    if (!dynRegion.loopBound.brokenOut) {
      SE_DPRINTF("[Stepper] NoEnd as LoopBound not BrokenOut.\n");
      return false;
    }
  }
  for (auto S : staticRegion.streams) {
    if (!S->isInnerFinalValueUsedByCore()) {
      continue;
    }
    auto &dynS = S->getDynStream(dynRegion.seqNum);
    assert(dynS.hasTotalTripCount());
    auto tripCount = dynS.getTotalTripCount();
    auto endElem = dynS.getFirstUnsteppedElem();
    if (!endElem) {
      SE_DPRINTF(
          "[Stepper] NoEnd as No FIrstUnstepElem Next %ld TripCount %ld.\n",
          dynS.FIFOIdx.entryIdx, tripCount);
      return false;
    }
    if (endElem->FIFOIdx.entryIdx != tripCount) {
      SE_DPRINTF(
          "[Stepper] NoEnd as FirstUnstepElem %s is not TripCount %ld.\n",
          endElem->FIFOIdx, tripCount);
      return false;
    }
    if (!endElem->isFirstUserDispatched()) {
      SE_DPRINTF(
          "[Stepper] NoEnd as CoreUser not Distpatched for EndElem %s.\n",
          endElem->FIFOIdx);
      return false;
    }
    auto iter = this->se->elementUserMap.find(endElem);
    if (iter != this->se->elementUserMap.end()) {
      const auto &users = iter->second;
      if (!users.empty()) {
        SE_DPRINTF(
            "[Stepper] NoEnd as CoreUser %lu not Committed for EndElem %s.\n",
            *users.begin(), endElem->FIFOIdx);
        return false;
      }
    }
  }

  bool canDispatch = this->canDispatchStreamEndImpl(staticRegion, dynRegion);
  if (!canDispatch) {
    SE_DPRINTF("[Stepper] NoDispatchEnd.\n");
    return false;
  }

  bool canExecute = this->canExecuteStreamEndImpl(staticRegion, dynRegion);
  if (!canExecute) {
    SE_DPRINTF("[Stepper] NoExecuteEnd.\n");
    return false;
  }

  bool canCommit = this->canCommitStreamEndImpl(staticRegion, dynRegion);
  if (!canCommit) {
    SE_DPRINTF("[Stepper] NoCommitEnd.\n");
    return false;
  }

  SE_DPRINTF("[Stepper] End %s ConfigSeqNum %lu.\n",
             staticRegion.region.region(), configSeqNum);

  this->dispatchStreamEndImpl(args, staticRegion, dynRegion);
  this->commitStreamEnd(args);

  return true;
}

void StreamRegionController::determineStepElemCount(const ConfigArgs &args) {

  /**
   * By default we step the element one by one. But this may change when
   * we want to optimize for:
   * 1. InnerReduction stream with LoopEliminated, and
   * 2. The computation is offloaded.
   */
  auto &dynRegion = *this->activeDynRegionMap.at(args.seqNum);

  if (se->myParams->enableRangeSync) {
    return;
  }
  if (!se->myParams->streamEngineEnableFloat) {
    return;
  }

  auto &staticRegion = *dynRegion.staticRegion;
  if (!staticRegion.someStreamsLoopEliminated) {
    return;
  }
  if (staticRegion.region.is_loop_bound()) {
    return;
  }
  // Can not skip if we have nest streams.
  if (!dynRegion.nestConfigs.empty()) {
    return;
  }

  const auto &staticGroups = staticRegion.step.stepGroups;
  auto &dynGroups = dynRegion.step.stepGroups;

  for (auto &dynGroup : dynGroups) {
    auto rootS = staticGroups[dynGroup.staticGroupIdx].stepRootS;
    for (auto S : this->se->getStepStreamList(rootS)) {
      auto &dynS = S->getDynStream(dynRegion.seqNum);
      // There is unfloated memory stream.
      if (S->isMemStream() && !dynS.isFloatedToCache()) {
        return;
      }
    }
  }
  for (auto &dynGroup : dynGroups) {

    auto S = staticGroups[dynGroup.staticGroupIdx].stepRootS;
    if (!S->isLoopEliminated()) {
      continue;
    }
    if (S->hasCoreUser()) {
      continue;
    }

    auto &dynS = S->getDynStream(dynRegion.seqNum);
    if (!dynS.hasTotalTripCount()) {
      continue;
    }
    if (!dynS.hasInnerTripCount()) {
      continue;
    }

    const auto &staticGroup = staticGroups.at(dynGroup.staticGroupIdx);
    if (staticGroup.needSecondFinalValue) {
      continue;
    }

    dynGroup.stepElemCount = dynS.getInnerTripCount();
    DYN_S_DPRINTF(dynS.dynStreamId, "[Stepper] Change StepElemCount to %ld.\n",
                  dynS.getInnerTripCount());

    for (auto stepS : this->se->getStepStreamList(S)) {
      auto &stepDynS =
          stepS->getDynStreamByInstance(dynS.dynStreamId.streamInstance);
      stepDynS.stepElemCount = stepDynS.getInnerTripCount();
    }
  }
}
} // namespace gem5
