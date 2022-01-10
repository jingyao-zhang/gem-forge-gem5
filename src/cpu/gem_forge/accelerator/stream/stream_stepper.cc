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
  bool needFinalValue = false;
  bool needSecondFinalValue = false;
  for (auto S : staticRegion.streams) {
    if (S->stepRootStream == S) {
      SE_DPRINTF("[Stepper] Add StepRootStream %s.\n", S->getStreamName());
      staticStep.stepRootStreams.push_back(S);
    }
    if (S->isFinalValueNeededByCore()) {
      SE_DPRINTF("[Stepper] NeedFinalValue %s.\n", S->getStreamName());
      needFinalValue = true;
    }
    if (S->isSecondFinalValueNeededByCore()) {
      SE_DPRINTF("[Stepper] NeedSecondFinalValue %s.\n", S->getStreamName());
      needSecondFinalValue = true;
    }
  }
  if (needSecondFinalValue && needFinalValue) {
    SE_PANIC(
        "[Stepper] Can't step for both FinalValue and SecondFinalValue %s.",
        region.region());
  }
  staticStep.needSecondFinalValue = needSecondFinalValue;

  if (staticStep.stepRootStreams.empty()) {
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
    int64_t totalTripCount = DynamicStream::InvalidTotalTripCount;
    for (auto S : staticRegion.streams) {
      auto &dynS = S->getDynamicStream(dynRegion.seqNum);
      if (!dynS.hasTotalTripCount()) {
        DYN_S_PANIC(
            dynS.dynamicStreamId,
            "[Stepper] EliminatedLoop w/o. LoopBound must has TotalTripCount.");
      }
      if (totalTripCount == DynamicStream::InvalidTotalTripCount) {
        totalTripCount = dynS.getTotalTripCount();
        DYN_S_DPRINTF(dynS.dynamicStreamId,
                      "[Stepper] Get TotalTripCount %lld.\n", totalTripCount);
      } else if (totalTripCount != dynS.getTotalTripCount()) {
        DYN_S_PANIC(dynS.dynamicStreamId,
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
  uint64_t stepOffset = staticStep.needSecondFinalValue ? 1 : 0;
  if (staticRegion.region.is_loop_bound()) {
    if (dynStep.nextElementIdx + stepOffset >= dynBound.nextElementIdx) {
      SE_DPRINTF("[Stepper] Wait For LoopBound: %llu + %llu >= %llu.\n",
                 dynStep.nextElementIdx, stepOffset, dynBound.nextElementIdx);
      return;
    }
  } else {
    // We don't have StreamLoopBound.
    auto firstStepRootS = staticStep.stepRootStreams.front();
    auto &firstStepRootDynS =
        firstStepRootS->getDynamicStream(dynRegion.seqNum);
    auto totalTripCount = firstStepRootDynS.getTotalTripCount();
    if (dynStep.nextElementIdx + stepOffset >= totalTripCount) {
      SE_DPRINTF("[Stepper] Wait For TotalTripCount: %llu + %llu >= %llu.\n",
                 dynStep.nextElementIdx, stepOffset, totalTripCount);
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
  auto stepRootS = staticStep.stepRootStreams.at(dynStep.nextStepStreamIdx);
  auto stepRootStreamId = stepRootS->staticId;
  auto &stepRootDynS = stepRootS->getDynamicStream(dynRegion.seqNum);
  if (dynStep.nextElementIdx >= stepRootDynS.getTotalTripCount()) {
    DYN_S_PANIC(stepRootDynS.dynamicStreamId,
                "[Stepper] Step Beyond TotalTripCount %lld >= %lld.",
                dynStep.nextElementIdx, stepRootDynS.getTotalTripCount());
  }

  StreamEngine::StreamStepArgs args(stepRootStreamId);
  args.dynInstanceId = stepRootDynS.dynamicStreamId.streamInstance;

  DYN_S_DPRINTF(stepRootDynS.dynamicStreamId,
                "[Stepper] Try to Step RootDynS.\n");

  switch (dynStep.state) {
  default:
    SE_PANIC("[Stepper] Invalid State %d.", dynStep.state);
  case DynRegion::DynStep::StepState::BEFORE_DISPATCH:
    if (se->canDispatchStreamStep(args)) {
      DYN_S_DPRINTF(stepRootDynS.dynamicStreamId, "[Stepper] Dispatch.\n");
      se->dispatchStreamStep(args);

      dynStep.state = DynRegion::DynStep::StepState::BEFORE_COMMIT;

    } else {
      DYN_S_DPRINTF(stepRootDynS.dynamicStreamId,
                    "[Stepper] CanNot Dispatch.\n");
    }
    break;
  case DynRegion::DynStep::StepState::BEFORE_COMMIT:
    if (se->canCommitStreamStep(args)) {
      DYN_S_DPRINTF(stepRootDynS.dynamicStreamId, "[Stepper] Commit.\n");
      se->commitStreamStep(args);

      dynStep.state = DynRegion::DynStep::StepState::BEFORE_DISPATCH;
      dynStep.nextStepStreamIdx++;
      if (dynStep.nextStepStreamIdx == staticStep.stepRootStreams.size()) {
        dynStep.nextStepStreamIdx = 0;
        dynStep.nextElementIdx++;
        DYN_S_DPRINTF(stepRootDynS.dynamicStreamId,
                      "[Stepper] NextElementIdx++ -> %llu.\n",
                      dynStep.nextElementIdx);
      }

    } else {
      DYN_S_DPRINTF(stepRootDynS.dynamicStreamId, "[Stepper] CanNot Commit.\n");
    }
  }
}