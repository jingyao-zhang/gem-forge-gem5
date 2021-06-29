#include "stream_region_controller.hh"

#include "base/trace.hh"
#include "debug/StreamLoopBound.hh"

#define DEBUG_TYPE StreamLoopBound
#include "stream_log.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamLoopBound, format, ##args)

void StreamRegionController::initializeStreamLoopBound(
    const ::LLVM::TDG::StreamRegion &region, StaticRegion &staticRegion) {
  if (!region.is_loop_bound()) {
    return;
  }

  const auto &boundFuncInfo = region.loop_bound_func();
  auto boundFunc = std::make_shared<TheISA::ExecFunc>(
      se->getCPUDelegator()->getSingleThreadContext(), boundFuncInfo);
  const bool boundRet = region.loop_bound_ret();

  SE_DPRINTF(
      "[LoopBound] Initialized StaticLoopBound for region %s. BoundRet %d.\n",
      region.region(), boundRet);
  auto &staticBound = staticRegion.loopBound;
  staticBound.boundFunc = boundFunc;
  staticBound.boundRet = boundRet;

  for (const auto &arg : region.loop_bound_func().args()) {
    if (arg.is_stream()) {
      // This is a stream input. Remember this in the base stream.
      auto S = this->se->getStream(arg.stream_id());
      staticBound.baseStreams.insert(S);
    }
  }

  SE_DPRINTF("[LoopBound] Initialized StaticLoopBound for region %s.\n",
             region.region());
}

void StreamRegionController::dispatchStreamConfigForLoopBound(
    const ConfigArgs &args, DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  if (!staticRegion.region.is_loop_bound()) {
    return;
  }
  dynRegion.loopBound.boundFunc = staticRegion.loopBound.boundFunc;
  SE_DPRINTF("[LoopBound] Dispatch DynLoopBound for region %s.\n",
             staticRegion.region.region());
}

void StreamRegionController::executeStreamConfigForLoopBound(
    const ConfigArgs &args, DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  if (!staticRegion.region.is_loop_bound()) {
    return;
  }
  auto &dynBound = dynRegion.loopBound;

  assert(args.inputMap && "Missing InputMap.");
  assert(args.inputMap->count(
             ::LLVM::TDG::ReservedStreamRegionId::LoopBoundFuncInputRegionId) &&
         "Missing InputVec for LoopBound.");
  auto &inputVec = args.inputMap->at(
      ::LLVM::TDG::ReservedStreamRegionId::LoopBoundFuncInputRegionId);

  int inputIdx = 0;

  // Construct the NestConfigFunc formal params.
  SE_DPRINTF("[LoopBound] Executed DynLoopBound for region %s.\n",
             staticRegion.region.region());
  {
    auto &formalParams = dynBound.formalParams;
    const auto &funcInfo = dynBound.boundFunc->getFuncInfo();
    SE_DPRINTF("[LoopBound] boundFunc %#x.\n", dynBound.boundFunc);
    this->buildFormalParams(inputVec, inputIdx, funcInfo, formalParams);
  }
  SE_DPRINTF("[LoopBound] Executed DynLoopBound for region %s.\n",
             staticRegion.region.region());
}

void StreamRegionController::checkLoopBound(DynRegion &dynRegion) {
  auto &staticRegion = *dynRegion.staticRegion;
  if (!staticRegion.region.is_loop_bound()) {
    return;
  }

  auto &staticBound = staticRegion.loopBound;
  auto &dynBound = dynRegion.loopBound;
  if (dynBound.brokenOut) {
    // We already reached the end of the loop.
    return;
  }

  auto nextElementIdx = dynBound.nextElementIdx;
  std::unordered_set<StreamElement *> baseElements;
  for (auto baseS : staticBound.baseStreams) {
    auto &baseDynS = baseS->getDynamicStream(dynRegion.seqNum);
    auto baseElement = baseDynS.getElementByIdx(nextElementIdx);
    if (!baseElement) {
      if (baseDynS.FIFOIdx.entryIdx > nextElementIdx) {
        DYN_S_PANIC(baseDynS.dynamicStreamId,
                    "[LoopBound] Miss Element %llu.\n", nextElementIdx);
      } else {
        // The base element is not allocated yet.
        DYN_S_DPRINTF(baseDynS.dynamicStreamId,
                      "[LoopBound] BaseElement %llu not Allocated.\n",
                      nextElementIdx);
        return;
      }
    }
    if (!baseElement->isValueReady) {
      S_ELEMENT_DPRINTF(baseElement, "[LoopBound] Not Ready.\n");
      return;
    }
    baseElements.insert(baseElement);
  }

  // All base elements are value ready.
  auto getStreamValue =
      GetStreamValueFromElementSet(baseElements, "[LoopBound]");

  auto actualParams =
      convertFormalParamToParam(dynBound.formalParams, getStreamValue);

  auto ret = dynBound.boundFunc->invoke(actualParams).front();
  if (ret == staticBound.boundRet) {
    /**
     * Should break out the loop.
     * So far we just set TotalTripCount for all DynStreams.
     */
    SE_DPRINTF("[LoopBound] Break (%d == %d) Region %s.\n", ret,
               staticBound.boundRet, staticRegion.region.region());
    dynBound.brokenOut = true;
    for (auto S : staticRegion.streams) {
      auto &dynS = S->getDynamicStream(dynRegion.seqNum);
      dynS.setTotalTripCount(dynBound.nextElementIdx + 1);
    }

  } else {
    // Keep going.
    SE_DPRINTF("[LoopBound] Continue (%d != %d) Region %s.\n", ret,
               staticBound.boundRet, staticRegion.region.region());
  }
  dynBound.nextElementIdx++;
}