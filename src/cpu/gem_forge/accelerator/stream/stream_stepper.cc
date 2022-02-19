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
  // DynStep is default initialized.
}

void StreamRegionController::executeStreamConfigForStep(const ConfigArgs &args,
                                                        DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  if (!staticRegion.region.loop_eliminated()) {
    return;
  }
  if (!staticRegion.region.is_loop_bound()) {
    // Without StreamLoopBound, we must check for TotalTripCount.
    int64_t totalTripCount = DynStream::InvalidTotalTripCount;
    for (auto S : staticRegion.streams) {
      auto &dynS = S->getDynStream(dynRegion.seqNum);
      if (!dynS.hasTotalTripCount()) {
        DYN_S_PANIC(
            dynS.dynStreamId,
            "[Stepper] EliminatedLoop w/o. LoopBound must has TotalTripCount.");
      }
      if (totalTripCount == DynStream::InvalidTotalTripCount) {
        totalTripCount = dynS.getTotalTripCount();
        DYN_S_DPRINTF(dynS.dynStreamId, "[Stepper] Get TotalTripCount %lld.\n",
                      totalTripCount);
      } else if (totalTripCount != dynS.getTotalTripCount()) {
        DYN_S_PANIC(dynS.dynStreamId,
                    "[Stepper] Mismatch TotalTripCount %lld != DynS %lld. Is "
                    "This Multi-Level Stream?",
                    totalTripCount, dynS.getTotalTripCount());
      }
    }
  }
}

void StreamRegionController::stepStream(DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  if (!staticRegion.region.loop_eliminated()) {
    return;
  }

  auto &staticStep = staticRegion.step;
  auto &dynStep = dynRegion.step;

  SE_DPRINTF("[Stepper] Try to Step Region %s.\n",
             staticRegion.region.region());

  /**
   * First check that we have handled possible NestStream and LoopBound
   * for the next stepped iteration.
   */
  const auto &dynBound = dynRegion.loopBound;
  if (staticRegion.region.is_loop_bound()) {
    if (dynStep.nextElementIdx >= dynBound.nextElementIdx) {
      SE_DPRINTF("[Stepper] Wait For LoopBound: %llu >= %llu.\n",
                 dynStep.nextElementIdx, dynBound.nextElementIdx);
      return;
    }
  } else {
    // We don't have StreamLoopBound.
    const auto &firstStepGroup = staticStep.stepGroups.front();
    auto firstStepRootS = firstStepGroup.stepRootS;
    auto &firstStepRootDynS =
        firstStepRootS->getDynStream(dynRegion.seqNum);
    auto totalTripCount = firstStepRootDynS.getTotalTripCount();
    if (dynStep.nextElementIdx >= totalTripCount) {
      SE_DPRINTF("[Stepper] Wait For TotalTripCount: %llu >= %llu.\n",
                 dynStep.nextElementIdx, totalTripCount);
      return;
    }
  }

  for (const auto &dynNestConfig : dynRegion.nestConfigs) {
    if (dynStep.nextElementIdx >= dynNestConfig.nextElementIdx) {
      SE_DPRINTF("[Stepper] Wait for NestRegion: %llu >= %llu Region %s.\n",
                 dynStep.nextElementIdx, dynNestConfig.nextElementIdx,
                 dynNestConfig.staticRegion->region.region());
      return;
    }
  }

  /**
   * Actually start to step.
   */
  const auto &stepGroup = staticStep.stepGroups.at(dynStep.nextStepStreamIdx);
  auto stepRootS = stepGroup.stepRootS;
  auto stepRootStreamId = stepRootS->staticId;
  auto &stepRootDynS = stepRootS->getDynStream(dynRegion.seqNum);
  if (dynStep.nextElementIdx >= stepRootDynS.getTotalTripCount()) {
    DYN_S_PANIC(stepRootDynS.dynStreamId,
                "[Stepper] Step Beyond TotalTripCount %lld >= %lld.",
                dynStep.nextElementIdx, stepRootDynS.getTotalTripCount());
  }

  StreamEngine::StreamStepArgs args(stepRootStreamId);
  args.dynInstanceId = stepRootDynS.dynStreamId.streamInstance;

  DYN_S_DPRINTF(stepRootDynS.dynStreamId, "[Stepper] Try to Step RootDynS.\n");

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
    if (stepGroup.needSecondFinalValue && stepRootDynS.hasTotalTripCount() &&
        dynStep.nextElementIdx + 1 == stepRootDynS.getTotalTripCount()) {
      DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                    "[Stepper] Skip Step SecondLast Element %lu.\n",
                    dynStep.nextElementIdx);
      dynStep.state = DynRegion::DynStep::StepState::BEFORE_DISPATCH;
      dynStep.nextStepStreamIdx++;
      if (dynStep.nextStepStreamIdx == staticStep.stepGroups.size()) {
        dynStep.nextStepStreamIdx = 0;
        dynStep.nextElementIdx++;
        DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                      "[Stepper] NextElementIdx++ -> %llu.\n",
                      dynStep.nextElementIdx);
      }
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

      dynStep.state = DynRegion::DynStep::StepState::BEFORE_DISPATCH;
      dynStep.nextStepStreamIdx++;
      if (dynStep.nextStepStreamIdx == staticStep.stepGroups.size()) {
        dynStep.nextStepStreamIdx = 0;
        dynStep.nextElementIdx++;
        DYN_S_DPRINTF(stepRootDynS.dynStreamId,
                      "[Stepper] NextElementIdx++ -> %llu.\n",
                      dynStep.nextElementIdx);
      }

    } else {
      DYN_S_DPRINTF(stepRootDynS.dynStreamId, "[Stepper] CanNot Commit.\n");
    }
    break;
  }
  }
}