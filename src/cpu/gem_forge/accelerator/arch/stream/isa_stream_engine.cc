#include "isa_stream_engine.hh"

#include "cpu/base.hh"
#include "cpu/exec_context.hh"
#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"
#include "debug/ISAStreamEngine.hh"
#include "proto/protoio.hh"

#if THE_ISA == RISCV_ISA
#include "arch/riscv/insts/standard.hh"
#endif

#define ISA_SE_PANIC(format, args...)                                          \
  panic("%llu-[ISA_SE%d] " format, cpuDelegator->curCycle(),                   \
        cpuDelegator->cpuId(), ##args)
#define ISA_SE_DPRINTF(format, args...)                                        \
  DPRINTF(ISAStreamEngine, "%llu-[ISA_SE%d] " format,                          \
          cpuDelegator->curCycle(), cpuDelegator->cpuId(), ##args)
#define DYN_INST_DPRINTF(format, args...)                                      \
  ISA_SE_DPRINTF("%llu %s " format, dynInfo.seqNum, dynInfo.pc, ##args)
#define DYN_INST_PANIC(format, args...)                                        \
  ISA_SE_PANIC("%llu %s " format, dynInfo.seqNum, dynInfo.pc, ##args)

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
    GemForgeLSQCallbackList &extraLSQCallbacks) {
  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  auto infoRelativePath = this->getRelativePath(configIdx);

  DYN_INST_DPRINTF("[dispatch] StreamConfig %llu, %s.\n", configIdx,
                   infoRelativePath);

  // Initialize the regionStreamId translation table.
  const auto &info = this->getStreamRegion(configIdx);

  /**
   * Allocate the current DynStreamRegionInfo.
   * Remember the inst info.
   */
  auto &instInfo = this->createDynStreamInstInfo(dynInfo.seqNum);
  auto &configInfo = instInfo.configInfo;
  configInfo.dynStreamRegionInfo = std::make_shared<DynStreamRegionInfo>(
      infoRelativePath, this->curStreamRegionInfo);
  this->curStreamRegionInfo = configInfo.dynStreamRegionInfo;

  this->curStreamRegionInfo->numDispatchedInsts++;

  if (configInfo.dynStreamRegionInfo->prevRegion) {
    DYN_INST_DPRINTF("[dispatch] MustMisspeculated StreamConfig %llu, %s: Has "
                     "previous region.\n",
                     configIdx, infoRelativePath);
    instInfo.mustBeMisspeculated = true;
    instInfo.mustBeMisspeculatedReason = CONFIG_HAS_PREV_REGION;
    instInfo.configInfo.dynStreamRegionInfo->mustBeMisspeculated = true;
    return;
  }

  if (this->hasRecursiveRegion(configIdx)) {
    DYN_INST_DPRINTF("[dispatch] MustMisspeculated StreamConfig %llu, %s: "
                     "Recursive region.\n",
                     configIdx, infoRelativePath);
    instInfo.mustBeMisspeculated = true;
    instInfo.mustBeMisspeculatedReason = CONFIG_RECURSIVE;
    instInfo.configInfo.dynStreamRegionInfo->mustBeMisspeculated = true;
    return;
  }

  if (!this->canSetRegionStreamIds(info)) {
    DYN_INST_DPRINTF(
        "[dispatch] MustMisspeculated StreamConfig %llu, %s: Cannot "
        "set region stream table.\n",
        configIdx, infoRelativePath);
    instInfo.mustBeMisspeculated = true;
    instInfo.mustBeMisspeculatedReason = CONFIG_CANNOT_SET_REGION_ID;
    instInfo.configInfo.dynStreamRegionInfo->mustBeMisspeculated = true;
    return;
  }
  this->insertRegionStreamIds(configIdx, info);

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
  // Also Initialize an empty InputVector for nest configuration.
  // ! This so far just reuse the InvalidStreamId.
  this->curStreamRegionInfo->inputMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(InvalidStreamId),
      std::forward_as_tuple());
}

bool ISAStreamEngine::canExecuteStreamConfig(
    const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::executeStreamConfig(const GemForgeDynInstInfo &dynInfo,
                                          ExecContext &xc) {
  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  auto infoRelativePath = this->getRelativePath(configIdx);
  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    return;
  }
  auto &configInfo = instInfo.configInfo;
  DYN_INST_DPRINTF(
      "[dispatch] StreamConfig Dispatched %d Executed %d ConfigIdx %llu, %s.\n",
      configInfo.dynStreamRegionInfo->numDispatchedInsts,
      configInfo.dynStreamRegionInfo->numExecutedInsts + 1, configIdx,
      infoRelativePath);
  instInfo.executed = true;
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
  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    panic("[commit] MustMisspeculated StreamConfig %llu, %s, Reason %s.",
          configIdx, infoRelativePath,
          mustBeMisspeculatedString(instInfo.mustBeMisspeculatedReason));
  }
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

void ISAStreamEngine::rewindStreamConfig(const GemForgeDynInstInfo &dynInfo) {
  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  auto infoRelativePath = this->getRelativePath(configIdx);
  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  auto &configInfo = this->getDynStreamInstInfo(dynInfo.seqNum).configInfo;
  DYN_INST_DPRINTF("[rewind] StreamConfig MustMisspeculated %d %llu, %s.\n",
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
  this->removeRegionStreamIds(configIdx, info);

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
    GemForgeLSQCallbackList &extraLSQCallbacks) {
  assert(this->curStreamRegionInfo && "Missing DynStreamRegionInfo.");
  this->curStreamRegionInfo->numDispatchedInsts++;

  // Remember the current DynStreamRegionInfo.
  auto &instInfo = this->createDynStreamInstInfo(dynInfo.seqNum);
  auto &configInfo = instInfo.configInfo;
  configInfo.dynStreamRegionInfo = this->curStreamRegionInfo;

  // Check if the previous StreamConfig is misspeculated.
  if (this->curStreamRegionInfo->mustBeMisspeculated) {
    DYN_INST_DPRINTF("[dispatch] MustMisspeculated StreamInput.\n");
    instInfo.mustBeMisspeculated = true;
    return;
  }

  // Remember the input info.
  auto &inputInfo = this->getDynStreamInstInfo(dynInfo.seqNum).inputInfo;
  // Translate the regionStreamId.
  auto regionStreamId = this->extractImm<uint64_t>(dynInfo.staticInst);
  auto streamId = this->lookupRegionStreamId(regionStreamId);
  inputInfo.translatedStreamId = streamId;

  // Allocate the entry in the InputMap.
  auto &inputMap = configInfo.dynStreamRegionInfo->inputMap;
  auto &inputVec = inputMap.at(streamId);
  inputInfo.inputIdx = inputVec.size();
  DYN_INST_DPRINTF(
      "[dispatch] StreamInput StreamId %llu #%d Dispatched %d Executed %d.\n",
      inputInfo.translatedStreamId, inputInfo.inputIdx,
      configInfo.dynStreamRegionInfo->numDispatchedInsts,
      configInfo.dynStreamRegionInfo->numExecutedInsts);
  inputVec.emplace_back();
}

bool ISAStreamEngine::canExecuteStreamInput(
    const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::executeStreamInput(const GemForgeDynInstInfo &dynInfo,
                                         ExecContext &xc) {

  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    DYN_INST_DPRINTF("[execute] MustMisspeculated StreamInput.\n");
    return;
  }

  auto &configInfo = instInfo.configInfo;

  // Record the live input.
  auto &inputInfo = instInfo.inputInfo;
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
    DYN_INST_DPRINTF("Record input StreamId %llu #%d-%d Val %llu.\n",
                     inputInfo.translatedStreamId, inputInfo.inputIdx, srcIdx,
                     regValue);
    inputValue[srcIdx] = regValue;
  }
  DYN_INST_DPRINTF(
      "[execute] StreamInput StreamId %llu #%d Dispatched %d Executed %d.\n",
      inputInfo.translatedStreamId, inputInfo.inputIdx,
      configInfo.dynStreamRegionInfo->numDispatchedInsts,
      configInfo.dynStreamRegionInfo->numExecutedInsts + 1);
  instInfo.executed = true;

  this->increamentStreamRegionInfoNumExecutedInsts(
      *(configInfo.dynStreamRegionInfo));
}

bool ISAStreamEngine::canCommitStreamInput(const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::commitStreamInput(const GemForgeDynInstInfo &dynInfo) {
  // Release the InstInfo.
  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    panic("[commit] MustMisspeculated StreamInput.\n");
  }
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

void ISAStreamEngine::rewindStreamInput(const GemForgeDynInstInfo &dynInfo) {
  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    DYN_INST_DPRINTF("[rewind] MustMisspeculated StreamInput.\n");
    this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
    return;
  }
  auto &configInfo = instInfo.configInfo;
  auto &inputInfo = instInfo.inputInfo;
  auto &regionInfo = configInfo.dynStreamRegionInfo;

  // Check if I executed.
  if (instInfo.executed) {
    regionInfo->numExecutedInsts--;
  }

  // Decrease numDispatchedInst.
  regionInfo->numDispatchedInsts--;

  // Release the inputVec.
  DYN_INST_DPRINTF(
      "[rewind] StreamInput StreamId %llu %#d Dispatched %d Executed %d.\n",
      inputInfo.translatedStreamId, inputInfo.inputIdx,
      regionInfo->numDispatchedInsts, regionInfo->numExecutedInsts);
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
    DYN_INST_DPRINTF("[canDispatch] MustMisspeculated StreamReady.\n");
    return true;
  }
  const auto &infoRelativePath = this->curStreamRegionInfo->infoRelativePath;
  ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum, infoRelativePath);
  auto se = this->getStreamEngine();
  if (se->canStreamConfig(args)) {
    DYN_INST_DPRINTF("[canDispatch] StreamReady %s.\n", infoRelativePath);
    return true;
  } else {
    DYN_INST_DPRINTF("[canNotDispatch] StreamReady %s.\n", infoRelativePath);
    return false;
  }
}

void ISAStreamEngine::dispatchStreamReady(
    const GemForgeDynInstInfo &dynInfo,
    GemForgeLSQCallbackList &extraLSQCallbacks) {
  assert(this->curStreamRegionInfo && "Missing DynStreamRegionInfo.");
  auto &instInfo = this->createDynStreamInstInfo(dynInfo.seqNum);
  // Remember the current DynStreamRegionInfo.
  auto &configInfo = instInfo.configInfo;
  configInfo.dynStreamRegionInfo = this->curStreamRegionInfo;

  if (this->curStreamRegionInfo->mustBeMisspeculated) {
    // Handle must be misspeculated.
    DYN_INST_DPRINTF("[dispatch] MustMisspeculated StreamReady.\n");
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

  this->curStreamRegionInfo->numDispatchedInsts++;
  this->curStreamRegionInfo->streamReadyDispatched = true;
  this->curStreamRegionInfo->streamReadySeqNum = dynInfo.seqNum;
  DYN_INST_DPRINTF("[dispatch] StreamReady Dispatched %d Executed %d %s.\n",
                   this->curStreamRegionInfo->numDispatchedInsts,
                   this->curStreamRegionInfo->numExecutedInsts,
                   infoRelativePath);
  // Release the current DynStreamRegionInfo.
  this->curStreamRegionInfo = nullptr;
}

bool ISAStreamEngine::canExecuteStreamReady(
    const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::executeStreamReady(const GemForgeDynInstInfo &dynInfo,
                                         ExecContext &xc) {
  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    DYN_INST_DPRINTF("[execute] MustMisspeculated StreamReady.\n");
    return;
  }

  auto &configInfo = instInfo.configInfo;

  DYN_INST_DPRINTF("[execute] StreamReady Dispatched %d Executed %d %s.\n",
                   configInfo.dynStreamRegionInfo->numDispatchedInsts,
                   configInfo.dynStreamRegionInfo->numExecutedInsts + 1,
                   configInfo.dynStreamRegionInfo->infoRelativePath);
  instInfo.executed = true;
  this->increamentStreamRegionInfoNumExecutedInsts(
      *(configInfo.dynStreamRegionInfo));
}

bool ISAStreamEngine::canCommitStreamReady(const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::commitStreamReady(const GemForgeDynInstInfo &dynInfo) {
  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    panic("[commit] MustMisspeculated StreamReady.\n");
  }

  // Notifiy the StreamEngine.
  auto &configInfo = instInfo.configInfo;
  auto infoRelativePath = configInfo.dynStreamRegionInfo->infoRelativePath;

  ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum, infoRelativePath);
  auto se = this->getStreamEngine();
  se->commitStreamConfig(args);
  DYN_INST_DPRINTF("[commit] StreamReady %s.\n", infoRelativePath);

  // Release the InstInfo.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

void ISAStreamEngine::rewindStreamReady(const GemForgeDynInstInfo &dynInfo) {
  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  auto &configInfo = instInfo.configInfo;
  auto &regionInfo = configInfo.dynStreamRegionInfo;

  if (instInfo.mustBeMisspeculated) {
    DYN_INST_DPRINTF("[rewind] MustMisspeculated StreamReady.\n");
    // Restore the currentStreamRegion.
    this->curStreamRegionInfo = regionInfo;

    // Release the InstInfo.
    this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
    return;
  }

  assert(regionInfo->streamReadyDispatched &&
         "StreamReady must be dispatched.");

  // Check if the StreamReady is actually executed.
  if (instInfo.executed) {
    regionInfo->numExecutedInsts--;
  }
  ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum,
                                        regionInfo->infoRelativePath,
                                        nullptr /* InputVec */, dynInfo.tc);
  auto se = this->getStreamEngine();
  se->rewindStreamConfig(args);

  // Decrease numDispatchedInst.
  regionInfo->streamReadyDispatched = false;
  regionInfo->numDispatchedInsts--;

  DYN_INST_DPRINTF("[rewind] StreamReady Dispatched %d Executed %d %s.\n",
                   regionInfo->numDispatchedInsts, regionInfo->numExecutedInsts,
                   regionInfo->infoRelativePath);

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
    DYN_INST_DPRINTF("[CanNotDispatch] StreamEnd %llu, %s: Unable to "
                     "RemoveRegionStreamIds.\n",
                     configIdx, infoRelativePath);
    return false;
  }

  ::StreamEngine::StreamEndArgs args(dynInfo.seqNum, infoRelativePath);
  auto se = this->getStreamEngine();
  if (se->hasUnsteppedElement(args)) {
    DYN_INST_DPRINTF("[CanDispatch] StreamEnd %llu, %s..\n", configIdx,
                     infoRelativePath);
    return true;
  } else {
    DYN_INST_DPRINTF(
        "[CanNotDispatch] StreamEnd %llu, %s: No UnsteppedElement.\n",
        configIdx, infoRelativePath);
    return false;
  }
}

void ISAStreamEngine::dispatchStreamEnd(
    const GemForgeDynInstInfo &dynInfo,
    GemForgeLSQCallbackList &extraLSQCallbacks) {
  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  const auto &infoRelativePath = this->getRelativePath(configIdx);

  DYN_INST_DPRINTF("[dispatch] StreamEnd %llu, %s.\n", configIdx,
                   infoRelativePath);

  const auto &info = this->getStreamRegion(configIdx);
  this->createDynStreamInstInfo(dynInfo.seqNum);

  assert(this->canRemoveRegionStreamIds(info) &&
         "Cannot remove RegionStreamIds for StreamEnd.");
  ::StreamEngine::StreamEndArgs args(dynInfo.seqNum, infoRelativePath);
  auto se = this->getStreamEngine();
  assert(se->hasUnsteppedElement(args) && "No UnsteppedElement for StreamEnd.");
  /**
   * ! For nest stream region, we don't really remove the RegionStreamIds.
   */
  if (!info.is_nest()) {
    this->removeRegionStreamIds(configIdx, info);
  }
  se->dispatchStreamEnd(args);
}

bool ISAStreamEngine::canExecuteStreamEnd(const GemForgeDynInstInfo &dynInfo) {
  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    return true;
  }
  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  const auto &infoRelativePath = this->getRelativePath(configIdx);

  DYN_INST_DPRINTF("[canExecute] StreamEnd %llu, %s.\n", configIdx,
                   infoRelativePath);

  auto se = this->getStreamEngine();
  ::StreamEngine::StreamEndArgs args(dynInfo.seqNum, infoRelativePath);
  return se->canExecuteStreamEnd(args);
}

void ISAStreamEngine::executeStreamEnd(const GemForgeDynInstInfo &dynInfo,
                                       ExecContext &xc) {
  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  instInfo.executed = true;
}

bool ISAStreamEngine::canCommitStreamEnd(const GemForgeDynInstInfo &dynInfo) {
  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    return true;
  }

  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  const auto &infoRelativePath = this->getRelativePath(configIdx);

  auto se = this->getStreamEngine();
  ::StreamEngine::StreamEndArgs args(dynInfo.seqNum, infoRelativePath);
  auto canCommit = se->canCommitStreamEnd(args);
  DYN_INST_DPRINTF("[canCommit] StreamEnd %llu, %s, CanCommit? %d.\n",
                   configIdx, infoRelativePath, canCommit);

  // Release the info.
  return canCommit;
}

void ISAStreamEngine::commitStreamEnd(const GemForgeDynInstInfo &dynInfo) {

  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  assert(!instInfo.mustBeMisspeculated &&
         "Try to commit a MustBeMisspeculated inst.");

  auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
  const auto &infoRelativePath = this->getRelativePath(configIdx);

  DYN_INST_DPRINTF("[commit] StreamEnd %llu, %s.\n", configIdx,
                   infoRelativePath);

  auto se = this->getStreamEngine();
  ::StreamEngine::StreamEndArgs args(dynInfo.seqNum, infoRelativePath);
  se->commitStreamEnd(args);

  // Release the info.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

void ISAStreamEngine::rewindStreamEnd(const GemForgeDynInstInfo &dynInfo) {

  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  if (!instInfo.mustBeMisspeculated) {
    // Really rewind the StreamEnd.
    auto configIdx = this->extractImm<uint64_t>(dynInfo.staticInst);
    const auto &infoRelativePath = this->getRelativePath(configIdx);

    // Don't forget to add back the removed region stream ids.
    // Unless this is a nest stream region.
    const auto &info = this->getStreamRegion(configIdx);
    if (!info.is_nest()) {
      this->insertRegionStreamIds(configIdx, info);
    }

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
  auto emplaceRet = this->seqNumToDynInfoMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(dynInfo.seqNum),
      std::forward_as_tuple());
  auto &instInfo = emplaceRet.first->second;
  auto &stepInstInfo = instInfo.stepInfo;
  if (emplaceRet.second) {
    // First time. Translate the regionStreamId.
    if (!this->isValidRegionStreamId(regionStreamId)) {
      DYN_INST_DPRINTF(
          "[canDispatch] MustMisspeculated StreamStep RegionStream %llu.\n",
          regionStreamId);
      instInfo.mustBeMisspeculatedReason =
          MustBeMisspeculatedReason::STEP_INVALID_REGION_ID;
      instInfo.mustBeMisspeculated = true;
    } else {
      auto streamId = this->lookupRegionStreamId(regionStreamId);
      stepInstInfo.translatedStreamId = streamId;
    }
  }

  if (instInfo.mustBeMisspeculated) {
    DYN_INST_DPRINTF(
        "[canDispatch] StreamStep RegionStream %lu Y as MustMisspeculated.\n",
        regionStreamId);
    return true;
  } else {
    auto streamId = stepInstInfo.translatedStreamId;
    auto se = this->getStreamEngine();
    bool canStep = se->canDispatchStreamStep(streamId);
    DYN_INST_DPRINTF(
        "[canDispatch] StreamStep RegionStream %lu CanDispatch %c.\n",
        regionStreamId, canStep ? 'Y' : 'N');
    return canStep;
  }
}

void ISAStreamEngine::dispatchStreamStep(
    const GemForgeDynInstInfo &dynInfo,
    GemForgeLSQCallbackList &extraLSQCallbacks) {
  auto regionStreamId = this->extractImm<uint64_t>(dynInfo.staticInst);

  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    return;
  }
  auto &stepInfo = instInfo.stepInfo;
  auto streamId = stepInfo.translatedStreamId;
  auto se = this->getStreamEngine();
  if (!se->canDispatchStreamStep(streamId)) {
    // This must be wrong.
    auto regionStreamId = this->extractImm<uint64_t>(dynInfo.staticInst);
    DYN_INST_DPRINTF("[dispatch] MustMisspeculated StreamStep RegionStreamId "
                     "%llu StreamId %llu.\n",
                     regionStreamId, streamId);
    instInfo.mustBeMisspeculated = true;
    return;
  }
  DYN_INST_DPRINTF("[dispatch] StreamStep RegionStream %llu.\n",
                   regionStreamId);
  se->dispatchStreamStep(streamId);
}

bool ISAStreamEngine::canExecuteStreamStep(const GemForgeDynInstInfo &dynInfo) {
  return true;
}

void ISAStreamEngine::executeStreamStep(const GemForgeDynInstInfo &dynInfo,
                                        ExecContext &xc) {
  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  instInfo.executed = true;
}

bool ISAStreamEngine::canCommitStreamStep(const GemForgeDynInstInfo &dynInfo) {
  const auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  if (instInfo.mustBeMisspeculated) {
    return true;
  }
  const auto &stepInfo = instInfo.stepInfo;
  auto streamId = stepInfo.translatedStreamId;
  auto se = this->getStreamEngine();
  bool canCommit = se->canCommitStreamStep(streamId);
  if (canCommit) {
    DYN_INST_DPRINTF("[canCommit] StreamStep.\n");
    return true;
  } else {
    DYN_INST_DPRINTF("[canNotCommit] StreamStep.\n");
    return false;
  }
}

void ISAStreamEngine::commitStreamStep(const GemForgeDynInstInfo &dynInfo) {
  const auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  auto regionStreamId = this->extractImm<uint64_t>(dynInfo.staticInst);
  if (instInfo.mustBeMisspeculated) {
    panic("Commit a MustBeMisspeculated StreamStep %llu, Reason: %s.\n",
          regionStreamId,
          mustBeMisspeculatedString(instInfo.mustBeMisspeculatedReason));
  }
  const auto &stepInfo = instInfo.stepInfo;
  auto streamId = stepInfo.translatedStreamId;
  auto se = this->getStreamEngine();
  se->commitStreamStep(streamId);
  DYN_INST_DPRINTF("[commit] StreamStep RegionStream %llu.\n", regionStreamId);

  // Release the info.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

void ISAStreamEngine::rewindStreamStep(const GemForgeDynInstInfo &dynInfo) {
  const auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
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
  return this->canDispatchStreamUser(dynInfo, false);
}

void ISAStreamEngine::dispatchStreamLoad(
    const GemForgeDynInstInfo &dynInfo,
    GemForgeLSQCallbackList &extraLSQCallbacks) {
  this->dispatchStreamUser(dynInfo, false, extraLSQCallbacks);
}

bool ISAStreamEngine::canExecuteStreamLoad(const GemForgeDynInstInfo &dynInfo) {
  return this->canExecuteStreamUser(dynInfo, false);
}

void ISAStreamEngine::executeStreamLoad(const GemForgeDynInstInfo &dynInfo,
                                        ExecContext &xc) {
  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  const auto &userInfo = instInfo.userInfo;

  if (instInfo.mustBeMisspeculated) {
    // This must be a misspeculated instruction.
    return;
  }

  std::vector<uint64_t> usedStreamIds{
      userInfo.translatedUsedStreamIds.at(0),
  };
  StreamEngine::StreamUserArgs::ValueVec values;
  values.reserve(usedStreamIds.size());
  StreamEngine::StreamUserArgs args(dynInfo.seqNum, dynInfo.pc.pc(),
                                    usedStreamIds, false, &values);
  auto se = this->getStreamEngine();
  se->executeStreamUser(args);
  DYN_INST_DPRINTF("Execute StreamLoad StreamId %llu destRegs %d.\n",
                   userInfo.translatedUsedStreamIds.at(0),
                   dynInfo.staticInst->numDestRegs());
  if (dynInfo.staticInst->numDestRegs() == 0) {
    panic("No DestRegs for StreamLoad at PC %#x.\n", dynInfo.pc.pc());
  }

  /**
   * We handle wider registers by checking the number of destination
   * registers.
   */
  RegVal *loadedPtr = reinterpret_cast<uint64_t *>(values.at(0).data());
  for (int destIdx = 0; destIdx < dynInfo.staticInst->numDestRegs();
       ++destIdx, loadedPtr++) {
    assert(destIdx <
               StreamEngine::StreamUserArgs::MaxElementSize / sizeof(RegVal) &&
           "Too many destination registers.");
    auto loadedValue = *loadedPtr;
    DYN_INST_DPRINTF("[%llu] Got value %llu, reg %d %s.\n",
                     userInfo.translatedUsedStreamIds.at(0), loadedValue,
                     destIdx, dynInfo.staticInst->destRegIdx(destIdx));
    if (dynInfo.staticInst->isFloating()) {
      xc.setFloatRegOperandBits(dynInfo.staticInst, destIdx, loadedValue);
    } else {
      xc.setIntRegOperand(dynInfo.staticInst, destIdx, loadedValue);
    }
  }
  instInfo.executed = true;
}

bool ISAStreamEngine::canCommitStreamLoad(const GemForgeDynInstInfo &dynInfo) {
  return this->canCommitStreamUser(dynInfo, false);
}

void ISAStreamEngine::commitStreamLoad(const GemForgeDynInstInfo &dynInfo) {
  this->commitStreamUser(dynInfo, false);
}

void ISAStreamEngine::rewindStreamLoad(const GemForgeDynInstInfo &dynInfo) {
  this->rewindStreamUser(dynInfo, false);
}

/********************************************************************************
 * StreamStore Handlers.
 *******************************************************************************/

bool ISAStreamEngine::canDispatchStreamStore(
    const GemForgeDynInstInfo &dynInfo) {
  return this->canDispatchStreamUser(dynInfo, true);
}

void ISAStreamEngine::dispatchStreamStore(
    const GemForgeDynInstInfo &dynInfo,
    GemForgeLSQCallbackList &extraLSQCallbacks) {
  this->dispatchStreamUser(dynInfo, true, extraLSQCallbacks);
}

bool ISAStreamEngine::canExecuteStreamStore(
    const GemForgeDynInstInfo &dynInfo) {
  return this->canExecuteStreamUser(dynInfo, true);
}

void ISAStreamEngine::executeStreamStore(const GemForgeDynInstInfo &dynInfo,
                                         ExecContext &xc) {
  // Value for StreamStore is passed from the callback. Nothing to do here.
  return;
}

bool ISAStreamEngine::canCommitStreamStore(const GemForgeDynInstInfo &dynInfo) {
  return this->canCommitStreamUser(dynInfo, true);
}

void ISAStreamEngine::commitStreamStore(const GemForgeDynInstInfo &dynInfo) {
  this->commitStreamUser(dynInfo, true);
}

void ISAStreamEngine::rewindStreamStore(const GemForgeDynInstInfo &dynInfo) {
  this->rewindStreamUser(dynInfo, true);
}

/********************************************************************************
 * StreamUser Handlers.
 *******************************************************************************/

bool ISAStreamEngine::canDispatchStreamUser(const GemForgeDynInstInfo &dynInfo,
                                            bool isStore) {

  auto regionStreamId = this->extractImm<uint64_t>(dynInfo.staticInst);

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
      // This must be a misspeculated StreamStore.
      DYN_INST_DPRINTF("MustMisspeculated %s invalid regionStream %llu.\n",
                       dynInfo.staticInst->getName(), regionStreamId);
      dynStreamInstInfo.mustBeMisspeculated = true;
      dynStreamInstInfo.mustBeMisspeculatedReason =
          MustBeMisspeculatedReason::USER_INVALID_REGION_ID;
    }
  }

  // Check if the stream engine has unstepped elements.
  if (dynStreamInstInfo.mustBeMisspeculated) {
    return true;
  } else {
    auto usedStreamId = userInfo.translatedUsedStreamIds.front();
    std::vector<uint64_t> usedStreamIds{
        usedStreamId,
    };
    StreamEngine::StreamUserArgs args(dynInfo.seqNum, dynInfo.pc.pc(),
                                      usedStreamIds, isStore);
    auto se = this->getStreamEngine();
    // It's possible that we don't have element if we have reached the limit.
    if (!se->canDispatchStreamUser(args)) {
      // We must wait.
      DYN_INST_DPRINTF("CanNotDispatch %s %llu: No Unstepped Element.\n",
                       dynInfo.staticInst->getName(), usedStreamId);
      return false;
    } else {
      if (se->hasIllegalUsedLastElement(args)) {
        // This is a use beyond the last element. Must be misspeculated.
        dynStreamInstInfo.mustBeMisspeculated = true;
        dynStreamInstInfo.mustBeMisspeculatedReason =
            MustBeMisspeculatedReason::USER_USING_LAST_ELEMENT;
        DYN_INST_DPRINTF(
            "MustMisspeculated %s %llu: Illegal Used Last Element.\n",
            dynInfo.staticInst->getName(), usedStreamId);
        return true;
      }
      DYN_INST_DPRINTF("[canDispatch] %s %llu.\n",
                       dynInfo.staticInst->getName(), usedStreamId);
      return true;
    }
  }
}

void ISAStreamEngine::dispatchStreamUser(
    const GemForgeDynInstInfo &dynInfo, bool isStore,
    GemForgeLSQCallbackList &extraLSQCallbacks) {

  auto &instInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  auto &userInfo = instInfo.userInfo;

  if (instInfo.mustBeMisspeculated) {
    // This is a must be misspeculated instruction.
    return;
  }

  std::vector<uint64_t> usedStreamIds{
      userInfo.translatedUsedStreamIds.at(0),
  };
  StreamEngine::StreamUserArgs args(dynInfo.seqNum, dynInfo.pc.pc(),
                                    usedStreamIds, isStore);
  auto se = this->getStreamEngine();
  // It's possible that this is misspeculated and we don't have element.
  if (!se->canDispatchStreamUser(args)) {
    DYN_INST_PANIC("[dicpatch] Should check canDispatchStreamUser before "
                   "dispatching %s %llu Seq %llu.\n",
                   dynInfo.staticInst->getName(), usedStreamIds.at(0),
                   dynInfo.seqNum);
    instInfo.mustBeMisspeculated = true;
    instInfo.mustBeMisspeculatedReason =
        MustBeMisspeculatedReason::USER_SE_CANNOT_DISPATCH;
  } else {
    se->dispatchStreamUser(args);
    // After dispatch, we get extra LQ callbacks.
    se->createStreamUserLSQCallbacks(args, extraLSQCallbacks);
    DYN_INST_DPRINTF("[dispatch] %s %llu Seq %llu: with callback %d.\n",
                     dynInfo.staticInst->getName(),
                     userInfo.translatedUsedStreamIds.at(0), dynInfo.seqNum,
                     (bool)(extraLSQCallbacks.front()));
  }
}

bool ISAStreamEngine::canExecuteStreamUser(const GemForgeDynInstInfo &dynInfo,
                                           bool isStore) {
  const auto &dynStreamInstInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  const auto &userInfo = dynStreamInstInfo.userInfo;

  if (dynStreamInstInfo.mustBeMisspeculated) {
    // This must be a misspeculated instruction.
    return true;
  }

  std::vector<uint64_t> usedStreamIds{
      userInfo.translatedUsedStreamIds.at(0),
  };
  StreamEngine::StreamUserArgs args(dynInfo.seqNum, dynInfo.pc.pc(),
                                    usedStreamIds, isStore);
  auto se = this->getStreamEngine();
  bool canExecute = se->areUsedStreamsReady(args);
  DYN_INST_DPRINTF("[canExecute] %s %llu %c.\n", dynInfo.staticInst->getName(),
                   userInfo.translatedUsedStreamIds.at(0),
                   canExecute ? 'Y' : 'N');
  return canExecute;
}

bool ISAStreamEngine::canCommitStreamUser(const GemForgeDynInstInfo &dynInfo,
                                          bool isStore) {
  return true;
}

void ISAStreamEngine::commitStreamUser(const GemForgeDynInstInfo &dynInfo,
                                       bool isStore) {
  const auto &dynStreamInstInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  const auto &userInfo = dynStreamInstInfo.userInfo;
  if (dynStreamInstInfo.mustBeMisspeculated) {
    // This must be a misspeculated instruction.
    panic(
        "MustMisspeculated %s %s StreamId %llu Seq %llu commit, reason %s.\n",
        dynInfo.pc, dynInfo.staticInst->getName(),
        userInfo.translatedUsedStreamIds.at(0), dynInfo.seqNum,
        mustBeMisspeculatedString(dynStreamInstInfo.mustBeMisspeculatedReason));
    return;
  }
  std::vector<uint64_t> usedStreamIds{
      userInfo.translatedUsedStreamIds.at(0),
  };
  StreamEngine::StreamUserArgs args(dynInfo.seqNum, dynInfo.pc.pc(),
                                    usedStreamIds, isStore);
  auto se = this->getStreamEngine();
  se->commitStreamUser(args);

  // Release the info.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

void ISAStreamEngine::rewindStreamUser(const GemForgeDynInstInfo &dynInfo,
                                       bool isStore) {
  const auto &dynStreamInstInfo = this->getDynStreamInstInfo(dynInfo.seqNum);
  const auto &userInfo = dynStreamInstInfo.userInfo;
  if (dynStreamInstInfo.mustBeMisspeculated) {
    // This must be a misspeculated instruction.
    // Nothing to do.
  } else {
    std::vector<uint64_t> usedStreamIds{
        userInfo.translatedUsedStreamIds.at(0),
    };
    StreamEngine::StreamUserArgs args(dynInfo.seqNum, dynInfo.pc.pc(),
                                      usedStreamIds, isStore);
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
ISAStreamEngine::getStreamRegion(const std::string &relativePath) const {
  auto iter = this->memorizedStreamRegionRelativePathMap.find(relativePath);
  if (iter == this->memorizedStreamRegionRelativePathMap.end()) {
    auto path = cpuDelegator->getTraceExtraFolder() + "/" + relativePath;
    ProtoInputStream istream(path);
    iter = this->memorizedStreamRegionRelativePathMap
               .emplace(std::piecewise_construct,
                        std::forward_as_tuple(relativePath),
                        std::forward_as_tuple())
               .first;
    if (!istream.read(iter->second)) {
      panic("Failed to read in the stream region from file %s.", path);
    }
  }
  return iter->second;
}

const ::LLVM::TDG::StreamRegion &
ISAStreamEngine::getStreamRegion(uint64_t configIdx) const {
  auto iter = this->memorizedStreamRegionIdMap.find(configIdx);
  if (iter == this->memorizedStreamRegionIdMap.end()) {
    auto relativePath = this->getRelativePath(configIdx);
    const auto &streamRegion = this->getStreamRegion(relativePath);
    iter = this->memorizedStreamRegionIdMap.emplace(configIdx, &streamRegion)
               .first;
  }
  return *iter->second;
}

std::vector<const ::LLVM::TDG::StreamInfo *>
ISAStreamEngine::collectStreamInfoInRegion(
    const ::LLVM::TDG::StreamRegion &region) const {
  std::vector<const ::LLVM::TDG::StreamInfo *> streams;
  for (const auto &streamInfo : region.streams()) {
    streams.push_back(&streamInfo);
  }
  // Check nest streams.
  assert(region.nest_region_relative_paths_size() <= 1 &&
         "Multiple nest regions is not supported.");
  for (const auto &nestConfigRelativePath :
       region.nest_region_relative_paths()) {
    const auto &nestRegion = this->getStreamRegion(nestConfigRelativePath);
    assert(nestRegion.nest_region_relative_paths_size() == 0 &&
           "Recursive nest is not supported.");
    for (const auto &streamInfo : nestRegion.streams()) {
      streams.push_back(&streamInfo);
    }
  }
  return streams;
}

void ISAStreamEngine::insertRegionStreamIds(
    uint64_t configIdx, const ::LLVM::TDG::StreamRegion &region) {
  // Add one region table to the stack.
  this->regionStreamIdTableStack.emplace_back(configIdx);
  auto &regionStreamIdTable = this->regionStreamIdTableStack.back();
  auto streamInfos = this->collectStreamInfoInRegion(region);
  for (const auto &streamInfo : streamInfos) {
    auto streamId = streamInfo->id();
    auto regionStreamId = streamInfo->region_stream_id();
    assert(regionStreamId < MaxNumRegionStreams &&
           "More than 128 streams in a region.");
    regionStreamIdTable.at(regionStreamId) = streamId;
  }
  if (Debug::ISAStreamEngine) {
    std::stringstream ss;
    ss << "Set RegionStreamId";
    for (const auto &streamInfo : region.streams()) {
      // Perform the removal.
      auto regionStreamId = streamInfo.region_stream_id();
      ss << ' ' << regionStreamId << "->"
         << regionStreamIdTable.at(regionStreamId);
    }
    ISA_SE_DPRINTF("%s.\n", ss.str());
  }
}

bool ISAStreamEngine::hasRecursiveRegion(uint64_t configIdx) {
  // -- An exception is region with the same configIdx, which means recursion
  // and we have no support for that yet.
  for (const auto &regionStreamIdTable : this->regionStreamIdTableStack) {
    if (regionStreamIdTable.configIdx == configIdx) {
      // Recursion found.
      return true;
    }
  }
  return false;
}

bool ISAStreamEngine::canSetRegionStreamIds(
    const ::LLVM::TDG::StreamRegion &region) {
  // Since we are now using a stack of RegionStreamTable, we allow
  // overriding previous RegionStreamId. This is for inter-procedure
  // streams.
  auto streamInfos = this->collectStreamInfoInRegion(region);
  for (const auto &streamInfo : streamInfos) {
    // Check if valid to be removed.
    auto regionStreamId = streamInfo->region_stream_id();
    if (regionStreamId >= MaxNumRegionStreams) {
      // Overflow RegionStreamId.
      return false;
    }
  }
  return true;
}

bool ISAStreamEngine::canRemoveRegionStreamIds(
    const ::LLVM::TDG::StreamRegion &region) {
  if (this->regionStreamIdTableStack.empty()) {
    return false;
  }
  // Check that the last table matches with the region.
  const auto &regionStreamIdTable = this->regionStreamIdTableStack.back();
  auto streamInfos = this->collectStreamInfoInRegion(region);
  for (const auto &streamInfo : streamInfos) {
    // Check if valid to be removed.
    auto streamId = streamInfo->id();
    auto regionStreamId = streamInfo->region_stream_id();
    if (regionStreamId >= MaxNumRegionStreams) {
      // Overflow RegionStreamId.
      return false;
    }
    if (regionStreamIdTable.at(regionStreamId) != streamId) {
      // RegionStreamId Compromised.
      return false;
    }
  }
  return true;
}

void ISAStreamEngine::removeRegionStreamIds(
    uint64_t configIdx, const ::LLVM::TDG::StreamRegion &region) {
  assert(this->canRemoveRegionStreamIds(region) &&
         "Can not remove region stream ids.");
  if (Debug::ISAStreamEngine) {
    std::stringstream ss;
    ss << "Clear RegionStreamId";
    const auto &regionStreamIdTable = this->regionStreamIdTableStack.back();
    for (const auto &streamInfo : region.streams()) {
      // Perform the removal.
      auto regionStreamId = streamInfo.region_stream_id();
      ss << ' ' << regionStreamId << "->"
         << regionStreamIdTable.at(regionStreamId);
    }
    ISA_SE_DPRINTF("%s.\n", ss.str());
  }
  // Simply pop the table stack.
  assert(this->regionStreamIdTableStack.back().configIdx == configIdx &&
         "Mismatch in poping stream region.");
  this->regionStreamIdTableStack.pop_back();
}

uint64_t ISAStreamEngine::searchRegionStreamId(int regionStreamId) const {
  if (regionStreamId >= MaxNumRegionStreams) {
    return InvalidStreamId;
  }
  if (this->regionStreamIdTableStack.empty()) {
    return InvalidStreamId;
  }
  // Search backwards.
  for (auto iter = this->regionStreamIdTableStack.crbegin(),
            end = this->regionStreamIdTableStack.crend();
       iter != end; ++iter) {
    auto streamId = iter->at(regionStreamId);
    if (streamId != InvalidStreamId) {
      return streamId;
    }
  }
  return InvalidStreamId;
}

bool ISAStreamEngine::isValidRegionStreamId(int regionStreamId) const {
  return this->searchRegionStreamId(regionStreamId) != InvalidStreamId;
}

uint64_t ISAStreamEngine::lookupRegionStreamId(int regionStreamId) const {
  /**
   * Some ReservedStreamRegionId is mapped to InvalidStreamId for now.
   * So far this is only for NestStreamConfig.
   */
  if (regionStreamId ==
      ::LLVM::TDG::ReservedStreamRegionId::NestConfigureFuncInputRegionId) {
    assert(::LLVM::TDG::ReservedStreamRegionId::NumReservedStreamRegionId ==
               1 &&
           "Too many reserved RegionStreamIds.");
    return InvalidStreamId;
  }

  auto streamId = this->searchRegionStreamId(regionStreamId);
  if (streamId == InvalidStreamId) {
    panic("RegionStreamId %d translated to InvalidStreamId.", regionStreamId);
  }
  return streamId;
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

ISAStreamEngine::DynStreamInstInfo &
ISAStreamEngine::getDynStreamInstInfo(uint64_t seqNum) {
  auto iter = this->seqNumToDynInfoMap.find(seqNum);
  if (iter == this->seqNumToDynInfoMap.end()) {
    inform("Failed to get DynStreamInstInfo for %llu.", seqNum);
    assert(false && "Failed to get DynStreamInstInfo.");
  }
  return iter->second;
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
      panic("Failed to read in the AllStreamRegions from file %s.", path);
    }
  }
  assert(configIdx < this->allStreamRegions->relative_paths_size() &&
         "ConfigIdx overflow.");
  return this->allStreamRegions->relative_paths(configIdx);
}

std::string
ISAStreamEngine::mustBeMisspeculatedString(MustBeMisspeculatedReason reason) {
#define CASE_REASON(reason)                                                    \
  case reason:                                                                 \
    return #reason
  switch (reason) {
    CASE_REASON(CONFIG_HAS_PREV_REGION);
    CASE_REASON(CONFIG_RECURSIVE);
    CASE_REASON(CONFIG_CANNOT_SET_REGION_ID);
    CASE_REASON(STEP_INVALID_REGION_ID);
    CASE_REASON(USER_INVALID_REGION_ID);
    CASE_REASON(USER_USING_LAST_ELEMENT);
    CASE_REASON(USER_SE_CANNOT_DISPATCH);
  default:
    return "UNKNOWN_REASON";
  }
#undef CASE_REASON
}

void ISAStreamEngine::takeOverBy(GemForgeCPUDelegator *newDelegator) {
  this->cpuDelegator = newDelegator;
  // Clear memorized StreamEngine, even though by our design this should not
  // change.
  this->SE = nullptr;
  this->SEMemorized = false;
}

void ISAStreamEngine::reset() {
  this->regionStreamIdTableStack.clear();
  this->seqNumToDynInfoMap.clear();
  this->curStreamRegionInfo = nullptr;
}