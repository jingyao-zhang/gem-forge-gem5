#include "isa_stream_engine.hh"

#include "cpu/base.hh"
#include "cpu/exec_context.hh"
#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"
#include "debug/ISAStreamEngine.hh"
#include "proto/protoio.hh"

#if THE_ISA == RISCV_ISA
#include "arch/riscv/insts/standard.hh"
#endif

#define ISA_SE_DPRINTF(format, args...) DPRINTF(ISAStreamEngine, format, ##args)

constexpr uint64_t ISAStreamEngine::InvalidStreamId;
constexpr int ISAStreamEngine::DynStreamUserInstInfo::MaxUsedStreams;

/********************************************************************************
 * StreamConfig Handlers.
 *******************************************************************************/

bool ISAStreamEngine::canDispatchStreamConfig(
    const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::dispatchStreamConfig(
    const GemForgeDynInstInfo &dynInfo,
    GemForgeLQCallbackList &extraLQCallbacks) {
  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  auto infoRelativePath = this->getRelativePath(configIdx);

  ISA_SE_DPRINTF("[dispatch] StreamConfig %llu, %s.\n", configIdx,
                 infoRelativePath);

  // Initialize the regionStreamId translation table.
  const auto &info = this->getStreamRegion(configIdx);

  /**
   * Allocate the current DynStreamRegionInfo.
   */
  // Remember the inst info.
  auto &instInfo = this->createDynStreamInstInfo(dynInfo.seqNum);
  auto &configInfo = instInfo.configInfo;
  configInfo.dynStreamRegionInfo = std::make_shared<DynStreamRegionInfo>(
      infoRelativePath, this->curStreamRegionInfo);
  this->curStreamRegionInfo = configInfo.dynStreamRegionInfo;

  this->curStreamRegionInfo->numDispatchedInsts++;

  if (configInfo.dynStreamRegionInfo->prevRegion) {
    ISA_SE_DPRINTF("[dispatch] MustMisspeculated StreamConfig %llu, %s: Has "
                   "previous region.\n",
                   configIdx, infoRelativePath);
    instInfo.mustBeMisspeculated = true;
    instInfo.configInfo.dynStreamRegionInfo->mustBeMisspeculated = true;
    return;
  }

  if (!this->canSetRegionStreamIds(info)) {
    ISA_SE_DPRINTF("[dispatch] MustMisspeculated StreamConfig %llu, %s: Cannot "
                   "set region stream table.\n",
                   configIdx, infoRelativePath);
    instInfo.mustBeMisspeculated = true;
    instInfo.configInfo.dynStreamRegionInfo->mustBeMisspeculated = true;
    return;
  }
  this->insertRegionStreamIds(info);

  // Initialize an empty InputVector for each configured stream.
  for (const auto &streamInfo : info.streams()) {
    auto streamId = streamInfo.id();
    auto inserted =
        this->curStreamRegionInfo->inputMap
            .emplace(std::piecewise_construct, std::forward_as_tuple(streamId),
                     std::forward_as_tuple())
            .second;
    assert(inserted && "InputVector already initialized.");
  }
}

bool ISAStreamEngine::canExecuteStreamConfig(
    const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::executeStreamConfig(const GemForgeDynInstInfo &dynInfo,
                                          ExecContext &xc) {
  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    return;
  }
  auto &configInfo = instInfo.configInfo;
  this->increamentStreamRegionInfoNumExecutedInsts(
      *(configInfo.dynStreamRegionInfo));
}

bool ISAStreamEngine::canCommitStreamConfig(
    const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::commitStreamConfig(const GemForgeDynInstInfo &dynInfo) {
  // Release the InstInfo.
  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  auto infoRelativePath = this->getRelativePath(configIdx);
  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    panic("[commit] MustMisspeculated StreamConfig %llu, %s.", configIdx,
          infoRelativePath);
  }
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

void ISAStreamEngine::rewindStreamConfig(const GemForgeDynInstInfo &dynInfo) {
  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  auto infoRelativePath = this->getRelativePath(configIdx);
  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  auto &configInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum).configInfo;
  ISA_SE_DPRINTF("[rewind] StreamConfig MustMisspeculated %d %llu, %s.\n",
                 instInfo.mustBeMisspeculated, configIdx, infoRelativePath);
  if (instInfo.mustBeMisspeculated) {
    // Simply do nothing.
    this->curStreamRegionInfo = configInfo.dynStreamRegionInfo->prevRegion;
    this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
    return;
  }

  // Check the current DynStreamRegionInfo.
  assert(this->curStreamRegionInfo == configInfo.dynStreamRegionInfo &&
         "Mismatch curStreamRegionInfo when rewinding StreamConfig.");
  assert(this->curStreamRegionInfo->numDispatchedInsts == 1 &&
         "More than one dispatched inst when rewinding StreamConfig.");
  assert(!this->curStreamRegionInfo->streamReadyDispatched &&
         "StreamReady should not be dispatched when rewinding StreamConfig.");

  /**
   * No need to notify the StreamEngine. It's StreamReady's job.
   * Just clear the curStreamRegionInfo.
   */
  this->curStreamRegionInfo = configInfo.dynStreamRegionInfo->prevRegion;
  assert(!this->curStreamRegionInfo && "Has previous stream region?");

  // Clear the regionStreamId translation table.
  const auto &info = this->getStreamRegion(configIdx);
  assert(this->canRemoveRegionStreamIds(info) &&
         "Failed rewinding StreamConfig");
  this->removeRegionStreamIds(info);

  // Release the InstInfo.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

/********************************************************************************
 * StreamInput Handlers.
 *******************************************************************************/

bool ISAStreamEngine::canDispatchStreamInput(
    const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::dispatchStreamInput(
    const GemForgeDynInstInfo &dynInfo,
    GemForgeLQCallbackList &extraLQCallbacks) {
  assert(this->curStreamRegionInfo && "Missing DynStreamRegionInfo.");
  this->curStreamRegionInfo->numDispatchedInsts++;

  // Remember the current DynStreamRegionInfo.
  auto &instInfo = this->createDynStreamInstInfo(dynInfo.seqNum);
  auto &configInfo = instInfo.configInfo;
  configInfo.dynStreamRegionInfo = this->curStreamRegionInfo;

  // Check if the previous StreamConfig is misspeculated.
  if (this->curStreamRegionInfo->mustBeMisspeculated) {
    ISA_SE_DPRINTF("[dispatch] MustMisspeculated StreamInput.\n");
    instInfo.mustBeMisspeculated = true;
    return;
  }

  // Remember the input info.
  auto &inputInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum).inputInfo;
  // Translate the regionStreamId.
  auto regionStreamId = this->extractImm<uint64_t>(dynInfo.staticInst);
  auto streamId = this->lookupRegionStreamId(regionStreamId);
  inputInfo.translatedStreamId = streamId;

  // Allocate the entry in the InputMap.
  auto &inputMap = configInfo.dynStreamRegionInfo->inputMap;
  auto &inputVec = inputMap.at(streamId);
  inputInfo.inputIdx = inputVec.size();
  ISA_SE_DPRINTF("[dispatch] StreamInput #%d %llu.\n", inputInfo.inputIdx,
                 inputInfo.translatedStreamId);
  inputVec.emplace_back(DynStreamRegionInfo::StreamInputValue{0});
}

bool ISAStreamEngine::canExecuteStreamInput(
    const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::executeStreamInput(const GemForgeDynInstInfo &dynInfo,
                                         ExecContext &xc) {

  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    ISA_SE_DPRINTF("[execute] MustMisspeculated StreamInput.\n");
    return;
  }

  auto &configInfo = instInfo.configInfo;

  // Record the live input.
  auto &inputInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum).inputInfo;
  auto &inputMap = configInfo.dynStreamRegionInfo->inputMap;
  auto &inputVec = inputMap.at(inputInfo.translatedStreamId);

  auto &inputValue = inputVec.at(inputInfo.inputIdx);
  for (int srcIdx = 0; srcIdx < dynInfo.staticInst->numSrcRegs(); ++srcIdx) {
    const auto &regId = dynInfo.staticInst->srcRegIdx(srcIdx);
    RegVal regValue = 0;
    if (regId.isIntReg()) {
      regValue = xc.readIntRegOperand(dynInfo.staticInst, srcIdx);
    } else {
      assert(regId.isFloatReg());
      regValue = xc.readFloatRegOperandBits(dynInfo.staticInst, srcIdx);
    }
    ISA_SE_DPRINTF("Record input %llu #%d-%d %llu.\n",
                   inputInfo.translatedStreamId, inputInfo.inputIdx, srcIdx,
                   regValue);
    inputValue[srcIdx] = regValue;
  }
  inputInfo.executed = true;

  this->increamentStreamRegionInfoNumExecutedInsts(
      *(configInfo.dynStreamRegionInfo));
}

bool ISAStreamEngine::canCommitStreamInput(const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::commitStreamInput(const GemForgeDynInstInfo &dynInfo) {
  // Release the InstInfo.
  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    panic("[commit] MustMisspeculated StreamInput.\n");
  }
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

void ISAStreamEngine::rewindStreamInput(const GemForgeDynInstInfo &dynInfo) {
  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    ISA_SE_DPRINTF("[rewind] MustMisspeculated StreamInput.\n");
    this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
    return;
  }
  auto &configInfo = instInfo.configInfo;
  auto &inputInfo = instInfo.inputInfo;
  auto &regionInfo = configInfo.dynStreamRegionInfo;

  // Check if I executed.
  if (inputInfo.executed) {
    regionInfo->numExecutedInsts--;
  }

  // Decrease numDispatchedInst.
  regionInfo->numDispatchedInsts--;

  // Release the inputVec.
  ISA_SE_DPRINTF("[rewind] StreamInput #%d %llu.\n", inputInfo.inputIdx,
                 inputInfo.translatedStreamId);
  auto &inputVec = regionInfo->inputMap.at(inputInfo.translatedStreamId);
  assert(inputVec.size() == inputInfo.inputIdx + 1 && "Mismatch input index.");
  inputVec.pop_back();

  // Release the InstInfo.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

/********************************************************************************
 * StreamReady Handlers.
 *******************************************************************************/

bool ISAStreamEngine::canDispatchStreamReady(
    const GemForgeDynInstInfo &dynInfo) {
  /**
   * Although confusing, but ssp.stream.ready is used as the synchronization
   * point with the StreamEngine.
   * dispatchStreamConfig should have allocated curStreamRegionInfo.
   * Notice that this assumes canDispatch() and dispatch() follows each other.
   */
  assert(this->curStreamRegionInfo && "Missing DynStreamRegionInfo.");
  if (this->curStreamRegionInfo->mustBeMisspeculated) {
    ISA_SE_DPRINTF("[canDispatch] MustMisspeculated StreamReady.\n");
    return true;
  }
  const auto &infoRelativePath = this->curStreamRegionInfo->infoRelativePath;
  ISA_SE_DPRINTF("[canDispatch] StreamReady %s.\n", infoRelativePath);

  ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum, infoRelativePath);
  auto se = this->getStreamEngine();
  return se->canStreamConfig(args);
}

void ISAStreamEngine::dispatchStreamReady(
    const GemForgeDynInstInfo &dynInfo,
    GemForgeLQCallbackList &extraLQCallbacks) {
  assert(this->curStreamRegionInfo && "Missing DynStreamRegionInfo.");
  auto &instInfo = this->createDynStreamInstInfo(dynInfo.seqNum);
  // Remember the current DynStreamRegionInfo.
  auto &configInfo = instInfo.configInfo;
  configInfo.dynStreamRegionInfo = this->curStreamRegionInfo;

  if (this->curStreamRegionInfo->mustBeMisspeculated) {
    // Handle must be misspeculated.
    ISA_SE_DPRINTF("[dispatch] MustMisspeculated StreamReady.\n");
    instInfo.mustBeMisspeculated = true;
    // Release the current DynStreamRegionInfo.
    this->curStreamRegionInfo = nullptr;
    return;
  }

  const auto &infoRelativePath = this->curStreamRegionInfo->infoRelativePath;
  ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum, infoRelativePath,
                                        nullptr /* InputVec */, dynInfo.tc);
  auto se = this->getStreamEngine();
  se->dispatchStreamConfig(args);
  ISA_SE_DPRINTF("[dispatch] StreamReady %s.\n", infoRelativePath.c_str());

  this->curStreamRegionInfo->numDispatchedInsts++;
  this->curStreamRegionInfo->streamReadyDispatched = true;
  this->curStreamRegionInfo->streamReadySeqNum = dynInfo.seqNum;
  // Release the current DynStreamRegionInfo.
  this->curStreamRegionInfo = nullptr;
}

bool ISAStreamEngine::canExecuteStreamReady(
    const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::executeStreamReady(const GemForgeDynInstInfo &dynInfo,
                                         ExecContext &xc) {
  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    ISA_SE_DPRINTF("[execute] MustMisspeculated StreamReady.\n");
    return;
  }

  auto &configInfo = instInfo.configInfo;
  this->increamentStreamRegionInfoNumExecutedInsts(
      *(configInfo.dynStreamRegionInfo));
}

bool ISAStreamEngine::canCommitStreamReady(const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::commitStreamReady(const GemForgeDynInstInfo &dynInfo) {
  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    panic("[commit] MustMisspeculated StreamReady.\n");
  }

  // Notifiy the StreamEngine.
  auto &configInfo = instInfo.configInfo;
  auto infoRelativePath = configInfo.dynStreamRegionInfo->infoRelativePath;

  ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum, infoRelativePath);
  auto se = this->getStreamEngine();
  se->commitStreamConfig(args);
  ISA_SE_DPRINTF("[commit] StreamReady %s.\n", infoRelativePath);

  // Release the InstInfo.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

void ISAStreamEngine::rewindStreamReady(const GemForgeDynInstInfo &dynInfo) {
  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  auto &configInfo = instInfo.configInfo;
  auto &regionInfo = configInfo.dynStreamRegionInfo;

  if (instInfo.mustBeMisspeculated) {
    ISA_SE_DPRINTF("[rewind] MustMisspeculated StreamReady.\n");
    // Restore the currentStreamRegion.
    this->curStreamRegionInfo = regionInfo;

    // Release the InstInfo.
    this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
    return;
  }

  assert(regionInfo->streamReadyDispatched &&
         "StreamReady must be dispatched.");

  ISA_SE_DPRINTF("[rewind] StreamReady %s.\n", regionInfo->infoRelativePath);

  // Check if the StreamReady is actually executed.
  if (regionInfo->numExecutedInsts == regionInfo->numDispatchedInsts) {
    // Too bad, we already called StreamEngine::executeStreamConfig().
    // This may be a little bit complicate to rewind.
    // ! This should never happen for MinorCPU.
    panic("%s when executeStreamConfig has been called is not implemented.",
          __PRETTY_FUNCTION__);
    regionInfo->numExecutedInsts--;
  } else {
    ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum,
                                          regionInfo->infoRelativePath,
                                          nullptr /* InputVec */, dynInfo.tc);
    auto se = this->getStreamEngine();
    se->rewindStreamConfig(args);
  }

  // Decrease numDispatchedInst.
  regionInfo->streamReadyDispatched = false;
  regionInfo->numDispatchedInsts--;

  // Restore the currentStreamRegion.
  this->curStreamRegionInfo = regionInfo;

  // Release the InstInfo.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

/********************************************************************************
 * StreamEnd Handlers.
 *******************************************************************************/

bool ISAStreamEngine::canDispatchStreamEnd(const GemForgeDynInstInfo &dynInfo) {
  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  const auto &infoRelativePath = this->getRelativePath(configIdx);

  ISA_SE_DPRINTF("CanDispatch StreamEnd %llu, %s.\n", configIdx,
                 infoRelativePath.c_str());
  /**
   * Sometimes it's possible to misspeculate StreamEnd before StreamConfig.
   * We check the RegionStreamIdTable to make sure this is the correct one.
   *
   * TODO: This is still very hacky, we have to be careful as there maybe
   * TODO: a misspeculated chain.
   */
  const auto &info = this->getStreamRegion(configIdx);
  if (!this->canRemoveRegionStreamIds(info)) {
    // We failed. This is must be misspeculation, we delay issue.
    return false;
  }

  ::StreamEngine::StreamEndArgs args(dynInfo.seqNum, infoRelativePath);
  auto se = this->getStreamEngine();
  return se->hasUnsteppedElement(args);
}

void ISAStreamEngine::dispatchStreamEnd(
    const GemForgeDynInstInfo &dynInfo,
    GemForgeLQCallbackList &extraLQCallbacks) {
  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  const auto &infoRelativePath = this->getRelativePath(configIdx);

  ISA_SE_DPRINTF("[dispatch] StreamEnd %llu, %s.\n", configIdx,
                 infoRelativePath.c_str());

  const auto &info = this->getStreamRegion(configIdx);
  this->createDynStreamInstInfo(dynInfo.seqNum);

  assert(this->canRemoveRegionStreamIds(info) &&
         "Cannot remove RegionStreamIds for StreamEnd.");
  ::StreamEngine::StreamEndArgs args(dynInfo.seqNum, infoRelativePath);
  auto se = this->getStreamEngine();
  assert(se->hasUnsteppedElement(args) && "No UnsteppedElement for StreamEnd.");
  // ! Make sure all effects happen as atomic.
  this->removeRegionStreamIds(info);
  se->dispatchStreamEnd(args);
}

bool ISAStreamEngine::canExecuteStreamEnd(const GemForgeDynInstInfo &dynInfo) {
  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    return true;
  }
  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  const auto &infoRelativePath = this->getRelativePath(configIdx);

  ISA_SE_DPRINTF("[canExecute] StreamEnd %llu, %s.\n", configIdx,
                 infoRelativePath.c_str());

  auto se = this->getStreamEngine();
  ::StreamEngine::StreamEndArgs args(dynInfo.seqNum, infoRelativePath);
  return se->canExecuteStreamEnd(args);
}

void ISAStreamEngine::executeStreamEnd(const GemForgeDynInstInfo &dynInfo,
                                       ExecContext &xc) {}

bool ISAStreamEngine::canCommitStreamEnd(const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::commitStreamEnd(const GemForgeDynInstInfo &dynInfo) {

  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  assert(!instInfo.mustBeMisspeculated &&
         "Try to commit a MustBeMisspeculated inst.");

  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  const auto &infoRelativePath = this->getRelativePath(configIdx);

  ISA_SE_DPRINTF("[commit] StreamEnd %llu, %s.\n", configIdx,
                 infoRelativePath.c_str());

  auto se = this->getStreamEngine();
  ::StreamEngine::StreamEndArgs args(dynInfo.seqNum, infoRelativePath);
  se->commitStreamEnd(args);

  // Release the info.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

void ISAStreamEngine::rewindStreamEnd(const GemForgeDynInstInfo &dynInfo) {

  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  if (!instInfo.mustBeMisspeculated) {
    // Really rewind the StreamEnd.
    auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
    const auto &infoRelativePath = this->getRelativePath(configIdx);

    // Don't forget to add back the removed region stream ids.
    const auto &info = this->getStreamRegion(configIdx);
    this->insertRegionStreamIds(info);

    auto se = this->getStreamEngine();
    ::StreamEngine::StreamEndArgs args(dynInfo.seqNum, infoRelativePath);
    se->rewindStreamEnd(args);
  }

  // Release the info.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

/********************************************************************************
 * StreamStep Handlers.
 *******************************************************************************/

bool ISAStreamEngine::canDispatchStreamStep(
    const GemForgeDynInstInfo &dynInfo) {
  // First create the memorized info.
  auto regionStreamId = this->extractImm<uint64_t>(dynInfo.staticInst);
  ISA_SE_DPRINTF("[canDispatch] StreamStep PC %#x RegionStream %lu.\n",
                 dynInfo.pc, regionStreamId);
  auto emplaceRet = this->seqNumToDynInfoMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(dynInfo.seqNum),
      std::forward_as_tuple());
  auto &instInfo = emplaceRet.first->second;
  auto &stepInstInfo = instInfo.stepInfo;
  if (emplaceRet.second) {
    // First time. Translate the regionStreamId.
    if (!this->isValidRegionStreamId(regionStreamId)) {
      ISA_SE_DPRINTF(
          "[canDispatch] MustMisspeculated StreamStep RegionStream %llu.\n",
          regionStreamId);
      instInfo.mustBeMisspeculated = true;
    } else {
      auto streamId = this->lookupRegionStreamId(regionStreamId);
      stepInstInfo.translatedStreamId = streamId;
    }
  }

  if (instInfo.mustBeMisspeculated) {
    return true;
  } else {
    auto streamId = stepInstInfo.translatedStreamId;
    auto se = this->getStreamEngine();
    return se->canStreamStep(streamId);
  }
}

void ISAStreamEngine::dispatchStreamStep(
    const GemForgeDynInstInfo &dynInfo,
    GemForgeLQCallbackList &extraLQCallbacks) {

  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    return;
  }
  auto &stepInfo = instInfo.stepInfo;
  auto streamId = stepInfo.translatedStreamId;
  auto se = this->getStreamEngine();
  if (!se->hasUnsteppedElement(streamId)) {
    // This must be wrong.
    auto regionStreamId = this->extractImm<uint64_t>(dynInfo.staticInst);
    ISA_SE_DPRINTF("[dispatch] MustMisspeculated StreamStep RegionStream %llu: "
                   "No unstepped elements.\n",
                   regionStreamId);
    instInfo.mustBeMisspeculated = true;
    return;
  }
  se->dispatchStreamStep(streamId);
}

bool ISAStreamEngine::canExecuteStreamStep(const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::executeStreamStep(const GemForgeDynInstInfo &dynInfo,
                                        ExecContext &xc) {}

bool ISAStreamEngine::canCommitStreamStep(const GemForgeDynInstInfo &dynInfo) {
  const auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    return true;
  }
  const auto &stepInfo = instInfo.stepInfo;
  auto streamId = stepInfo.translatedStreamId;
  auto se = this->getStreamEngine();
  return se->canCommitStreamStep(streamId);
}

void ISAStreamEngine::commitStreamStep(const GemForgeDynInstInfo &dynInfo) {
  const auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    auto regionStreamId = this->extractImm<uint64_t>(dynInfo.staticInst);
    panic("Commit a MustBeMisspeculated StreamStep %llu.\n", regionStreamId);
  }
  const auto &stepInfo = instInfo.stepInfo;
  auto streamId = stepInfo.translatedStreamId;
  auto se = this->getStreamEngine();
  se->commitStreamStep(streamId);

  // Release the info.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

void ISAStreamEngine::rewindStreamStep(const GemForgeDynInstInfo &dynInfo) {
  const auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    // Nothing to do.
  } else {
    const auto &stepInfo = instInfo.stepInfo;
    auto streamId = stepInfo.translatedStreamId;
    auto se = this->getStreamEngine();
    se->rewindStreamStep(streamId);
  }

  // Release the info.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

/********************************************************************************
 * StreamLoad Handlers.
 *******************************************************************************/

bool ISAStreamEngine::canDispatchStreamLoad(
    const GemForgeDynInstInfo &dynInfo) {

  auto regionStreamId = this->extractImm<uint64_t>(dynInfo.staticInst);
  ISA_SE_DPRINTF("CanDispatch StreamLoad RegionStream %llu.\n", regionStreamId);

  // First create the memorized info.
  auto emplaceRet = this->seqNumToDynInfoMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(dynInfo.seqNum),
      std::forward_as_tuple());
  auto &dynStreamInstInfo = emplaceRet.first->second;
  auto &userInfo = dynStreamInstInfo.userInfo;
  if (emplaceRet.second) {
    // First time. Translate the regionStreamId.
    if (this->isValidRegionStreamId(regionStreamId)) {
      auto streamId = this->lookupRegionStreamId(regionStreamId);
      userInfo.translatedUsedStreamIds.at(0) = streamId;
    } else {
      // This must be a misspeculated StreamLoad.
      ISA_SE_DPRINTF(
          "MustMisspeculated StreamLoad invalid regionStream %llu.\n",
          regionStreamId);
      dynStreamInstInfo.mustBeMisspeculated = true;
    }
  }

  // Check if the stream engine has unstepped elements.
  if (dynStreamInstInfo.mustBeMisspeculated) {
    return true;
  } else {
    std::vector<uint64_t> usedStreamIds{
        userInfo.translatedUsedStreamIds.at(0),
    };
    StreamEngine::StreamUserArgs args(dynInfo.seqNum, usedStreamIds);
    auto se = this->getStreamEngine();
    // It's possible that we don't have element if we have reached the limit.
    if (!se->hasUnsteppedElement(args)) {
      // We must wait.
      return false;
    } else {
      if (se->hasIllegalUsedLastElement(args)) {
        // This is a use beyond the last element. Must be misspeculated.
        dynStreamInstInfo.mustBeMisspeculated = true;
        return true;
      }
      // TODO: Check LSQ entry if this is the first use of the element.
      return true;
    }
  }
}

void ISAStreamEngine::dispatchStreamLoad(
    const GemForgeDynInstInfo &dynInfo,
    GemForgeLQCallbackList &extraLQCallbacks) {

  auto &instInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  auto &userInfo = instInfo.userInfo;

  if (instInfo.mustBeMisspeculated) {
    // This is a must be misspeculated instruction.
    return;
  }

  std::vector<uint64_t> usedStreamIds{
      userInfo.translatedUsedStreamIds.at(0),
  };
  StreamEngine::StreamUserArgs args(dynInfo.seqNum, usedStreamIds);
  auto se = this->getStreamEngine();
  // It's possible that this is misspeculated and we don't have element.
  if (!se->hasUnsteppedElement(args)) {
    panic("Should check hasUnsteppedElement before dispatching StreamLoad.");
    ISA_SE_DPRINTF("MustMisspeculated StreamLoad %llu Seq %llu: No element.\n",
                   usedStreamIds.at(0), dynInfo.seqNum);
    instInfo.mustBeMisspeculated = true;
  } else {
    se->dispatchStreamUser(args);
    // After dispatch, we get extra LQ callbacks.
    se->createStreamUserLQCallbacks(args, extraLQCallbacks);
    ISA_SE_DPRINTF("Dispatch StreamLoad %llu Seq %llu: with callback %d.\n",
                   userInfo.translatedUsedStreamIds.at(0), dynInfo.seqNum,
                   (bool)(extraLQCallbacks.front()));
  }
}

bool ISAStreamEngine::canExecuteStreamLoad(const GemForgeDynInstInfo &dynInfo) {
  const auto &dynStreamInstInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  const auto &userInfo = dynStreamInstInfo.userInfo;

  if (dynStreamInstInfo.mustBeMisspeculated) {
    // This must be a misspeculated instruction.
    return true;
  }

  std::vector<uint64_t> usedStreamIds{
      userInfo.translatedUsedStreamIds.at(0),
  };
  StreamEngine::StreamUserArgs args(dynInfo.seqNum, usedStreamIds);
  auto se = this->getStreamEngine();
  bool canExecute = se->areUsedStreamsReady(args);
  ISA_SE_DPRINTF("CanExecute StreamLoad %llu, %d.\n",
                 userInfo.translatedUsedStreamIds.at(0), canExecute);
  return canExecute;
}

void ISAStreamEngine::executeStreamLoad(const GemForgeDynInstInfo &dynInfo,
                                        ExecContext &xc) {
  const auto &dynStreamInstInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  const auto &userInfo = dynStreamInstInfo.userInfo;

  if (dynStreamInstInfo.mustBeMisspeculated) {
    // This must be a misspeculated instruction.
    return;
  }

  std::vector<uint64_t> usedStreamIds{
      userInfo.translatedUsedStreamIds.at(0),
  };
  StreamEngine::StreamUserArgs::ValueVec values;
  values.reserve(usedStreamIds.size());
  StreamEngine::StreamUserArgs args(dynInfo.seqNum, usedStreamIds, &values);
  auto se = this->getStreamEngine();
  se->executeStreamUser(args);
  ISA_SE_DPRINTF("Execute StreamLoad RegionStream %llu destRegs %d.\n",
                 userInfo.translatedUsedStreamIds.at(0),
                 dynInfo.staticInst->numDestRegs());

  /**
   * We handle wider registers by checking the number of destination registers.
   */
  RegVal *loadedPtr = reinterpret_cast<uint64_t *>(values.at(0).data());
  for (int destIdx = 0; destIdx < dynInfo.staticInst->numDestRegs();
       ++destIdx, loadedPtr++) {
    assert(destIdx <
               StreamEngine::StreamUserArgs::MaxElementSize / sizeof(RegVal) &&
           "Too many destination registers.");
    auto loadedValue = *loadedPtr;
    ISA_SE_DPRINTF("[%llu] Got value %llu, reg %d %s.\n",
                   userInfo.translatedUsedStreamIds.at(0), loadedValue, destIdx,
                   dynInfo.staticInst->destRegIdx(destIdx));
    if (dynInfo.staticInst->isFloating()) {
      xc.setFloatRegOperandBits(dynInfo.staticInst, destIdx, loadedValue);
    } else {
      xc.setIntRegOperand(dynInfo.staticInst, destIdx, loadedValue);
    }
  }
}

bool ISAStreamEngine::canCommitStreamLoad(const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::commitStreamLoad(const GemForgeDynInstInfo &dynInfo) {
  const auto &dynStreamInstInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  const auto &userInfo = dynStreamInstInfo.userInfo;
  if (dynStreamInstInfo.mustBeMisspeculated) {
    // This must be a misspeculated instruction.
    panic("MustMisspeculated StreamLoad %llu Seq %llu commit.\n",
          userInfo.translatedUsedStreamIds.at(0), dynInfo.seqNum);
    return;
  }
  std::vector<uint64_t> usedStreamIds{
      userInfo.translatedUsedStreamIds.at(0),
  };
  StreamEngine::StreamUserArgs args(dynInfo.seqNum, usedStreamIds);
  auto se = this->getStreamEngine();
  se->commitStreamUser(args);

  // Release the info.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

void ISAStreamEngine::rewindStreamLoad(const GemForgeDynInstInfo &dynInfo) {
  const auto &dynStreamInstInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  const auto &userInfo = dynStreamInstInfo.userInfo;
  if (dynStreamInstInfo.mustBeMisspeculated) {
    // This must be a misspeculated instruction.
    // Nothing to do.
  } else {
    std::vector<uint64_t> usedStreamIds{
        userInfo.translatedUsedStreamIds.at(0),
    };
    StreamEngine::StreamUserArgs args(dynInfo.seqNum, usedStreamIds);
    auto se = this->getStreamEngine();
    se->rewindStreamUser(args);
  }

  // Release the info.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

/********************************************************************************
 * APIs related to misspeculation handling.
 *******************************************************************************/
void ISAStreamEngine::storeTo(Addr vaddr, int size) {
  auto se = this->getStreamEngine();
  if (se) {
    se->cpuStoreTo(vaddr, size);
  }
}

/********************************************************************************
 * StreamEngine Helpers.
 *******************************************************************************/

::StreamEngine *ISAStreamEngine::getStreamEngine() {
  if (!this->SEMemorized) {
    this->SE =
        this->cpuDelegator->baseCPU->getAccelManager()->getStreamEngine();
    this->SEMemorized = true;
  }
  return this->SE;
}

template <typename T>
T ISAStreamEngine::extractImm(const StaticInst *staticInst) const {
#if THE_ISA == RISCV_ISA
  auto immOp = dynamic_cast<const RiscvISA::ImmOp<T> *>(staticInst);
  assert(immOp && "Invalid ImmOp.");
  return immOp->getImm();
#elif THE_ISA == X86_ISA
  auto machineInst = staticInst->machInst;
  return machineInst.immediate;
#else
  panic("ISA stream engine is not supported.");
#endif
}

const ::LLVM::TDG::StreamRegion &
ISAStreamEngine::getStreamRegion(uint64_t configIdx) const {
  auto iter = this->memorizedStreamRegionMap.find(configIdx);
  if (iter == this->memorizedStreamRegionMap.end()) {
    auto relativePath = this->getRelativePath(configIdx);
    auto path = cpuDelegator->getTraceExtraFolder() + "/" + relativePath;
    iter =
        this->memorizedStreamRegionMap
            .emplace(std::piecewise_construct, std::forward_as_tuple(configIdx),
                     std::forward_as_tuple())
            .first;
    ProtoInputStream istream(path);
    if (!istream.read(iter->second)) {
      panic("Failed to read in the stream region from file %s.", path.c_str());
    }
  }

  return iter->second;
}

void ISAStreamEngine::insertRegionStreamIds(
    const ::LLVM::TDG::StreamRegion &region) {
  for (const auto &streamInfo : region.streams()) {
    auto streamId = streamInfo.id();
    auto regionStreamId = streamInfo.region_stream_id();
    const int MaxRegionStreamId = 128;
    assert(regionStreamId < MaxRegionStreamId &&
           "More than 128 streams in a region.");
    this->regionStreamIdTable.at(regionStreamId) = streamId;
  }
  if (Debug::ISAStreamEngine) {
    std::stringstream ss;
    ss << "Set RegionStreamId";
    for (const auto &streamInfo : region.streams()) {
      // Perform the removal.
      auto regionStreamId = streamInfo.region_stream_id();
      ss << ' ' << regionStreamId << "->"
         << this->regionStreamIdTable.at(regionStreamId);
    }
    ISA_SE_DPRINTF("%s.\n", ss.str());
  }
}

bool ISAStreamEngine::canSetRegionStreamIds(
    const ::LLVM::TDG::StreamRegion &region) {
  for (const auto &streamInfo : region.streams()) {
    // Check if valid to be removed.
    auto regionStreamId = streamInfo.region_stream_id();
    if (this->regionStreamIdTable.size() <= regionStreamId) {
      // Overflow RegionStreamId.
      return false;
    }
    if (this->regionStreamIdTable.at(regionStreamId) != InvalidStreamId) {
      // RegionStreamId Compromised.
      return false;
    }
  }
  return true;
}

bool ISAStreamEngine::canRemoveRegionStreamIds(
    const ::LLVM::TDG::StreamRegion &region) {
  for (const auto &streamInfo : region.streams()) {
    // Check if valid to be removed.
    auto streamId = streamInfo.id();
    auto regionStreamId = streamInfo.region_stream_id();
    if (this->regionStreamIdTable.size() <= regionStreamId) {
      // Overflow RegionStreamId.
      return false;
    }
    if (this->regionStreamIdTable.at(regionStreamId) != streamId) {
      // RegionStreamId Compromised.
      return false;
    }
  }
  return true;
}

void ISAStreamEngine::removeRegionStreamIds(
    const ::LLVM::TDG::StreamRegion &region) {
  assert(this->canRemoveRegionStreamIds(region) &&
         "Can not remove region stream ids.");
  if (Debug::ISAStreamEngine) {
    std::stringstream ss;
    ss << "Clear RegionStreamId";
    for (const auto &streamInfo : region.streams()) {
      // Perform the removal.
      auto regionStreamId = streamInfo.region_stream_id();
      ss << ' ' << regionStreamId << "->"
         << this->regionStreamIdTable.at(regionStreamId);
    }
    ISA_SE_DPRINTF("%s.\n", ss.str());
  }
  for (const auto &streamInfo : region.streams()) {
    // Perform the removal.
    auto regionStreamId = streamInfo.region_stream_id();
    this->regionStreamIdTable.at(regionStreamId) = InvalidStreamId;
  }
}

bool ISAStreamEngine::isValidRegionStreamId(int regionStreamId) const {
  if (this->regionStreamIdTable.size() <= regionStreamId) {
    return false;
  }
  auto streamId = this->regionStreamIdTable.at(regionStreamId);
  if (streamId == InvalidStreamId) {
    return false;
  }
  return true;
}

uint64_t ISAStreamEngine::lookupRegionStreamId(int regionStreamId) const {
  if (!this->isValidRegionStreamId(regionStreamId)) {
    panic("RegionStreamId %d translated to InvalidStreamId.", regionStreamId);
  }
  return this->regionStreamIdTable.at(regionStreamId);
}

ISAStreamEngine::DynStreamInstInfo &
ISAStreamEngine::createDynStreamInstInfo(uint64_t seqNum) {
  auto emplaceRet = this->seqNumToDynInfoMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(seqNum),
      std::forward_as_tuple());
  assert(emplaceRet.second && "StreamInstInfo already there.");
  return emplaceRet.first->second;
}

ISAStreamEngine::DynStreamInstInfo &
ISAStreamEngine::getOrCreateDynStreamInstInfo(uint64_t seqNum) {
  auto emplaceRet = this->seqNumToDynInfoMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(seqNum),
      std::forward_as_tuple());
  return emplaceRet.first->second;
}

void ISAStreamEngine::increamentStreamRegionInfoNumExecutedInsts(
    DynStreamRegionInfo &dynStreamRegionInfo) {
  dynStreamRegionInfo.numExecutedInsts++;
  if (dynStreamRegionInfo.streamReadyDispatched &&
      dynStreamRegionInfo.numExecutedInsts ==
          dynStreamRegionInfo.numDispatchedInsts) {
    // We can notify the StreamEngine that StreamConfig can be executed,
    // including the InputMap.
    ::StreamEngine::StreamConfigArgs args(dynStreamRegionInfo.streamReadySeqNum,
                                          dynStreamRegionInfo.infoRelativePath,
                                          &dynStreamRegionInfo.inputMap);
    auto se = this->getStreamEngine();
    se->executeStreamConfig(args);
  }
}

const std::string &ISAStreamEngine::getRelativePath(int configIdx) const {
  if (!this->allStreamRegions) {
    auto path = cpuDelegator->getTraceExtraFolder() + "/all.stream.data";
    ProtoInputStream istream(path);
    this->allStreamRegions = m5::make_unique<::LLVM::TDG::AllStreamRegions>();
    if (!istream.read(*this->allStreamRegions)) {
      panic("Failed to read in the AllStreamRegions from file %s.",
            path.c_str());
    }
  }
  assert(configIdx < this->allStreamRegions->relative_paths_size() &&
         "ConfigIdx overflow.");
  return this->allStreamRegions->relative_paths(configIdx);
}
