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

void StreamRegionController::initializeStep(
    const ::LLVM::TDG::StreamRegion &region, StaticRegion &staticRegion) {

  // Just remember the StepRootStreams.
  SE_DPRINTF("[Stepper] Initialized StaticStep for region %s.\n",
             region.region());
  auto &staticStep = staticRegion.step;
  for (auto S : staticRegion.streams) {
    if (S->stepRootStream == S) {
      SE_DPRINTF("[Stepper] Add StepRootStream %s.\n", S->getStreamName());
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
    if (S->isFinalValueNeededByCore()) {
      SE_DPRINTF("[Stepper] NeedFinalValue %s.\n", S->getStreamName());
      stepGroup->needFinalValue = true;
    }
    if (S->isSecondFinalValueNeededByCore()) {
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
    SE_PANIC("[Stepper] No StepRootStream for region %s.", region.region());
  }
}

void StreamRegionController::dispatchStreamConfigForStep(const ConfigArgs &args,
                                                         DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  if (!staticRegion.region.loop_eliminated()) {
    return;
  }
  SE_DPRINTF("[Stepper] Initialized DynStep for region %s.\n",
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
  if (!staticRegion.region.loop_eliminated()) {
    return;
  }
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
        DYN_S_PANIC(
            dynS.dynStreamId,
            "[Stepper] EliminatedLoop w/o. LoopBound must has TotalTripCount.");
      }

      dynGroup.totalTripCount = dynS.getTotalTripCount();
      DYN_S_DPRINTF(dynS.dynStreamId, "[Stepper] Get TotalTripCount %ld.\n",
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
                      "[Stepper] Removed due to Zero TotalTripCount.\n");
        iter = dynGroups.erase(iter);
      }
    }

    // Compute and sanity check for LevelTripCount.
    for (int groupIdx = 0; groupIdx < dynGroups.size(); ++groupIdx) {
      auto &dynGroup = dynGroups[groupIdx];
      auto S = staticGroups[dynGroup.staticGroupIdx].stepRootS;
      auto &dynS = S->getDynStream(dynRegion.seqNum);
      if (groupIdx > 0) {
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
            if (!this->canSkipToStreamEnd(dynRegion)) {
              DYN_S_PANIC(dynS.dynStreamId,
                          "[Stepper] Mismatch TripCount in same LoopLevel %lld "
                          "!= %lld.",
                          prevDynGroup.totalTripCount, dynGroup.totalTripCount);
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
          assert(prevDynGroup.totalTripCount % dynGroup.totalTripCount == 0);
          auto levelTripCount =
              prevDynGroup.totalTripCount / dynGroup.totalTripCount;
          prevDynGroup.levelTripCount = levelTripCount;
          for (int i = groupIdx - 2; i >= 0; --i) {
            if (dynGroups[i].loopLevel != prevDynGroup.loopLevel) {
              break;
            }
            dynGroups[i].levelTripCount = levelTripCount;
          }
        }
      }
    }

    // Print the TripCount.
    for (int groupIdx = 0; groupIdx < dynGroups.size(); ++groupIdx) {
      auto &dynGroup = dynGroups[groupIdx];
      auto S = staticGroups[dynGroup.staticGroupIdx].stepRootS;
      auto &dynS = S->getDynStream(dynRegion.seqNum);
      DYN_S_DPRINTF(
          dynS.dynStreamId,
          "[Stepper] LoopLevel %d TotalTripCount %lld LevelTripCount %lld.\n",
          dynGroup.loopLevel, dynGroup.totalTripCount, dynGroup.levelTripCount);
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

void StreamRegionController::stepStream(DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  if (!staticRegion.region.loop_eliminated()) {
    return;
  }

  auto &dynStep = dynRegion.step;
  auto &dynGroup = dynStep.stepGroups[dynStep.nextDynGroupIdx];
  auto &staticStep = staticRegion.step;
  auto &staticGroup = staticStep.stepGroups[dynGroup.staticGroupIdx];

  SE_DPRINTF("[Stepper] Try to Step Region %s.\n",
             staticRegion.region.region());

  /**
   * First check that we have handled possible NestStream and LoopBound
   * for the next stepped iteration.
   */
  const auto &dynBound = dynRegion.loopBound;
  if (staticRegion.region.is_loop_bound()) {
    if (dynGroup.nextElemIdx >= dynBound.nextElemIdx) {
      SE_DPRINTF("[Stepper] Wait For LoopBound: %llu >= %llu.\n",
                 dynGroup.nextElemIdx, dynBound.nextElemIdx);
      return;
    }
  } else {
    // We don't have StreamLoopBound.
    if (dynGroup.nextElemIdx >= dynGroup.totalTripCount) {
      SE_DPRINTF("[Stepper] Wait For TotalTripCount: %llu >= %llu.\n",
                 dynGroup.nextElemIdx, dynGroup.totalTripCount);
      return;
    }
  }

  for (const auto &dynNestConfig : dynRegion.nestConfigs) {
    if (dynGroup.nextElemIdx >= dynNestConfig.nextElemIdx) {
      SE_DPRINTF("[Stepper] Wait for NestRegion: %llu >= %llu Region %s.\n",
                 dynGroup.nextElemIdx, dynNestConfig.nextElemIdx,
                 dynNestConfig.staticRegion->region.region());
      return;
    }
  }

  /**
   * Actually start to step.
   */
  auto stepRootS = staticGroup.stepRootS;
  auto stepRootStreamId = stepRootS->staticId;
  auto &stepRootDynS = stepRootS->getDynStream(dynRegion.seqNum);
  if (dynGroup.nextElemIdx >= stepRootDynS.getTotalTripCount()) {
    DYN_S_PANIC(stepRootDynS.dynStreamId,
                "[Stepper] Step Beyond TotalTripCount %lld >= %lld.",
                dynGroup.nextElemIdx, stepRootDynS.getTotalTripCount());
  }

  StreamEngine::StreamStepArgs args(stepRootStreamId);
  args.dynInstanceId = stepRootDynS.dynStreamId.streamInstance;

  DYN_S_DPRINTF(stepRootDynS.dynStreamId, "[Stepper] Try to Step RootDynS.\n");

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
    dynGroup.nextElemIdx++;
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

    if (se->canDispatchStreamStep(args)) {
      DYN_S_DPRINTF(stepRootDynS.dynStreamId, "[Stepper] Dispatch.\n");
      se->dispatchStreamStep(args);

      dynStep.state = DynRegion::DynStep::StepState::BEFORE_COMMIT;

    } else {
      DYN_S_DPRINTF(stepRootDynS.dynStreamId, "[Stepper] CanNot Dispatch.\n");
    }
    break;
  }
  case DynRegion::DynStep::StepState::BEFORE_COMMIT: {
    if (se->canCommitStreamStep(args)) {
      DYN_S_DPRINTF(stepRootDynS.dynStreamId, "[Stepper] Commit.\n");
      se->commitStreamStep(args);

      stepToNextGroup();

    } else {
      DYN_S_DPRINTF(stepRootDynS.dynStreamId, "[Stepper] CanNot Commit.\n");
    }
    break;
  }
  }
}