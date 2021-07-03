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
      staticStep.stepRootStreams.push_back(S);
    }
  }

  if (staticStep.stepRootStreams.empty()) {
    SE_PANIC("[Stepper] No StepRootStream for region %s.\n", region.region());
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
  if (dynStep.nextElementIdx >= dynBound.nextElementIdx) {
    SE_DPRINTF("[Stepper] Wait For LoopBound: %llu >= %llu.\n",
               dynStep.nextElementIdx, dynBound.nextElementIdx);
    return;
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

  SE_DPRINTF("[Stepper] Try to Step RootS %s.\n", stepRootS->getStreamName());

  switch (dynStep.state) {
  default:
    SE_PANIC("[Stepper] Invalid State %d.", dynStep.state);
  case DynRegion::DynStep::StepState::BEFORE_DISPATCH:
    if (se->canDispatchStreamStep(stepRootStreamId)) {
      SE_DPRINTF("[Stepper] Dispatch.\n");
      se->dispatchStreamStep(stepRootStreamId);

      dynStep.state = DynRegion::DynStep::StepState::BEFORE_COMMIT;

    } else {
      SE_DPRINTF("[Stepper] CanNot Dispatch.\n");
    }
    break;
  case DynRegion::DynStep::StepState::BEFORE_COMMIT:
    if (se->canCommitStreamStep(stepRootStreamId)) {
      SE_DPRINTF("[Stepper] Commit.\n");
      se->commitStreamStep(stepRootStreamId);

      dynStep.state = DynRegion::DynStep::StepState::BEFORE_DISPATCH;
      dynStep.nextStepStreamIdx++;
      if (dynStep.nextStepStreamIdx == staticStep.stepRootStreams.size()) {
        dynStep.nextStepStreamIdx = 0;
        dynStep.nextElementIdx++;
        SE_DPRINTF("[Stepper] NextElementIdx++ -> %llu.\n",
                   dynStep.nextElementIdx);
      }

    } else {
      SE_DPRINTF("[Stepper] CanNot Commit.\n");
    }
  }
}