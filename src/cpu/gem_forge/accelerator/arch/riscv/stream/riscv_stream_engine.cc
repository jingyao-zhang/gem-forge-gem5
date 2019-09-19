#include "riscv_stream_engine.hh"

#include "arch/riscv/insts/standard.hh"

#include "cpu/base.hh"
#include "cpu/exec_context.hh"
#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"
#include "proto/protoio.hh"

namespace RiscvISA {

constexpr uint64_t RISCVStreamEngine::InvalidStreamId;
constexpr int RISCVStreamEngine::DynStreamUserInstInfo::MaxUsedStreams;

/********************************************************************************
 * StreamConfig Handlers.
 *******************************************************************************/

bool RISCVStreamEngine::canDispatchStreamConfig(
    const GemForgeDynInstInfo &dynInfo, ExecContext &xc) {
  return true;
}

void RISCVStreamEngine::dispatchStreamConfig(const GemForgeDynInstInfo &dynInfo,
                                             ExecContext &xc) {
  auto rs1 = xc.readIntRegOperand(dynInfo.staticInst, 0);
  auto cpuDelegator = xc.tcBase()->getCpuPtr()->getCPUDelegator();
  assert(cpuDelegator && "Failed to find the CPUDelegator.");
  auto infoRelativePath = cpuDelegator->readStringFromMem(rs1);

  // Initialize the regionStreamId translation table.
  auto infoFullPath =
      cpuDelegator->getTraceExtraFolder() + "/" + infoRelativePath;
  const auto &info = this->getStreamRegion(infoFullPath);
  this->insertRegionStreamIds(info);

  /**
   * Allocate the current DynStreamRegionInfo.
   */
  assert(this->curStreamRegionInfo == nullptr &&
         "Previous DynStreamRegionInfo is not released yet.");
  this->curStreamRegionInfo =
      std::make_shared<DynStreamRegionInfo>(infoRelativePath);
  this->curStreamRegionInfo->numDispatchedInsts++;

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

  // Remember the inst info.
  auto &configInfo = this->createDynStreamInstInfo(dynInfo.seqNum).configInfo;
  configInfo.dynStreamRegionInfo = this->curStreamRegionInfo;
}

bool RISCVStreamEngine::canExecuteStreamConfig(
    const GemForgeDynInstInfo &dynInfo, ExecContext &xc) {
  return true;
}

void RISCVStreamEngine::executeStreamConfig(const GemForgeDynInstInfo &dynInfo,
                                            ExecContext &xc) {
  auto &configInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum).configInfo;
  this->increamentStreamRegionInfoNumExecutedInsts(
      *(configInfo.dynStreamRegionInfo), xc);
}

void RISCVStreamEngine::commitStreamConfig(const GemForgeDynInstInfo &dynInfo,
                                           ExecContext &xc) {
  // Release the InstInfo.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

/********************************************************************************
 * StreamInput Handlers.
 *******************************************************************************/

bool RISCVStreamEngine::canDispatchStreamInput(
    const GemForgeDynInstInfo &dynInfo, ExecContext &xc) {
  return true;
}

void RISCVStreamEngine::dispatchStreamInput(const GemForgeDynInstInfo &dynInfo,
                                            ExecContext &xc) {
  assert(this->curStreamRegionInfo && "Missing DynStreamRegionInfo.");
  this->curStreamRegionInfo->numDispatchedInsts++;

  // Remember the current DynStreamRegionInfo.
  auto &configInfo = this->createDynStreamInstInfo(dynInfo.seqNum).configInfo;
  configInfo.dynStreamRegionInfo = this->curStreamRegionInfo;

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
  inputVec.push_back(0);
}

bool RISCVStreamEngine::canExecuteStreamInput(
    const GemForgeDynInstInfo &dynInfo, ExecContext &xc) {
  return true;
}

void RISCVStreamEngine::executeStreamInput(const GemForgeDynInstInfo &dynInfo,
                                           ExecContext &xc) {
  auto &configInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum).configInfo;
  this->increamentStreamRegionInfoNumExecutedInsts(
      *(configInfo.dynStreamRegionInfo), xc);

  // Record the live input.
  auto &inputInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum).inputInfo;
  auto &inputMap = configInfo.dynStreamRegionInfo->inputMap;
  auto &inputVec = inputMap.at(inputInfo.translatedStreamId);
  auto rs1 = xc.readIntRegOperand(dynInfo.staticInst, 0);
  hack("Record input %llu.\n", rs1);
  inputVec.at(inputInfo.inputIdx) = rs1;
}

void RISCVStreamEngine::commitStreamInput(const GemForgeDynInstInfo &dynInfo,
                                          ExecContext &xc) {
  // Release the InstInfo.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

/********************************************************************************
 * StreamReady Handlers.
 *******************************************************************************/

bool RISCVStreamEngine::canDispatchStreamReady(
    const GemForgeDynInstInfo &dynInfo, ExecContext &xc) {
  /**
   * Although confusing, but ssp.stream.ready is used as the synchronization
   * point with the StreamEngine.
   * dispatchStreamConfig should have allocated curStreamRegionInfo.
   */
  assert(this->curStreamRegionInfo && "Missing DynStreamRegionInfo.");
  const auto &infoRelativePath = this->curStreamRegionInfo->infoRelativePath;

  ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum, infoRelativePath);
  auto se = this->getStreamEngine(xc);
  return se->canStreamConfig(args);
}

void RISCVStreamEngine::dispatchStreamReady(const GemForgeDynInstInfo &dynInfo,
                                            ExecContext &xc) {
  assert(this->curStreamRegionInfo && "Missing DynStreamRegionInfo.");
  const auto &infoRelativePath = this->curStreamRegionInfo->infoRelativePath;
  ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum, infoRelativePath);
  auto se = this->getStreamEngine(xc);
  se->dispatchStreamConfig(args);

  this->curStreamRegionInfo->numDispatchedInsts++;
  this->curStreamRegionInfo->streamReadyDispatched = true;
  this->curStreamRegionInfo->streamReadySeqNum = dynInfo.seqNum;

  // Remember the current DynStreamRegionInfo.
  auto &configInfo = this->createDynStreamInstInfo(dynInfo.seqNum).configInfo;
  configInfo.dynStreamRegionInfo = this->curStreamRegionInfo;

  // Release the current DynStreamRegionInfo.
  this->curStreamRegionInfo = nullptr;
}

bool RISCVStreamEngine::canExecuteStreamReady(
    const GemForgeDynInstInfo &dynInfo, ExecContext &xc) {
  return true;
}

void RISCVStreamEngine::executeStreamReady(const GemForgeDynInstInfo &dynInfo,
                                           ExecContext &xc) {
  auto &configInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum).configInfo;
  this->increamentStreamRegionInfoNumExecutedInsts(
      *(configInfo.dynStreamRegionInfo), xc);
}

void RISCVStreamEngine::commitStreamReady(const GemForgeDynInstInfo &dynInfo,
                                          ExecContext &xc) {
  // Notifiy the StreamEngine.
  auto &configInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum).configInfo;
  auto infoRelativePath = configInfo.dynStreamRegionInfo->infoRelativePath;

  ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum, infoRelativePath);
  auto se = this->getStreamEngine(xc);
  se->commitStreamConfig(args);

  // Release the InstInfo.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

/********************************************************************************
 * StreamEnd Handlers.
 *******************************************************************************/

bool RISCVStreamEngine::canDispatchStreamEnd(const GemForgeDynInstInfo &dynInfo,
                                             ExecContext &xc) {
  return true;
}

void RISCVStreamEngine::dispatchStreamEnd(const GemForgeDynInstInfo &dynInfo,
                                          ExecContext &xc) {
  auto se = this->getStreamEngine(xc);
  auto rs1 = xc.readIntRegOperand(dynInfo.staticInst, 0);
  auto cpuDelegator = xc.tcBase()->getCpuPtr()->getCPUDelegator();
  assert(cpuDelegator && "Failed to find the CPUDelegator.");
  auto infoRelativePath = cpuDelegator->readStringFromMem(rs1);

  auto infoFullPath =
      cpuDelegator->getTraceExtraFolder() + "/" + infoRelativePath;
  const auto &info = this->getStreamRegion(infoFullPath);
  this->removeRegionStreamIds(info);

  ::StreamEngine::StreamEndArgs args(dynInfo.seqNum, infoRelativePath);
  se->dispatchStreamEnd(args);
}

bool RISCVStreamEngine::canExecuteStreamEnd(const GemForgeDynInstInfo &dynInfo,
                                            ExecContext &xc) {
  return true;
}

void RISCVStreamEngine::executeStreamEnd(const GemForgeDynInstInfo &dynInfo,
                                         ExecContext &xc) {}

void RISCVStreamEngine::commitStreamEnd(const GemForgeDynInstInfo &dynInfo,
                                        ExecContext &xc) {
  auto se = this->getStreamEngine(xc);
  auto rs1 = xc.readIntRegOperand(dynInfo.staticInst, 0);
  auto cpuDelegator = xc.tcBase()->getCpuPtr()->getCPUDelegator();
  assert(cpuDelegator && "Failed to find the CPUDelegator.");
  auto infoRelativePath = cpuDelegator->readStringFromMem(rs1);
  ::StreamEngine::StreamEndArgs args(dynInfo.seqNum, infoRelativePath);
  se->commitStreamEnd(args);
}

/********************************************************************************
 * StreamStep Handlers.
 *******************************************************************************/

bool RISCVStreamEngine::canDispatchStreamStep(
    const GemForgeDynInstInfo &dynInfo, ExecContext &xc) {
  // First create the memorized info.
  auto emplaceRet = this->seqNumToDynInfoMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(dynInfo.seqNum),
      std::forward_as_tuple());
  auto &stepInstInfo = emplaceRet.first->second.stepInfo;
  if (emplaceRet.second) {
    // First time. Translate the regionStreamId.
    auto regionStreamId = this->extractImm<uint64_t>(dynInfo.staticInst);
    auto streamId = this->lookupRegionStreamId(regionStreamId);
    stepInstInfo.translatedStreamId = streamId;
  }

  auto streamId = stepInstInfo.translatedStreamId;

  auto se = this->getStreamEngine(xc);
  return se->canStreamStep(streamId);
}

void RISCVStreamEngine::dispatchStreamStep(const GemForgeDynInstInfo &dynInfo,
                                           ExecContext &xc) {

  const auto &dynStreamInstInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  const auto &stepInfo = dynStreamInstInfo.stepInfo;
  auto streamId = stepInfo.translatedStreamId;
  auto se = this->getStreamEngine(xc);
  se->dispatchStreamStep(streamId);
}

bool RISCVStreamEngine::canExecuteStreamStep(const GemForgeDynInstInfo &dynInfo,
                                             ExecContext &xc) {
  return true;
}

void RISCVStreamEngine::executeStreamStep(const GemForgeDynInstInfo &dynInfo,
                                          ExecContext &xc) {}

void RISCVStreamEngine::commitStreamStep(const GemForgeDynInstInfo &dynInfo,
                                         ExecContext &xc) {
  const auto &dynStreamInstInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  const auto &stepInfo = dynStreamInstInfo.stepInfo;
  auto streamId = stepInfo.translatedStreamId;
  auto se = this->getStreamEngine(xc);
  se->commitStreamStep(streamId);

  // Release the info.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

/********************************************************************************
 * StreamLoad Handlers.
 *******************************************************************************/

bool RISCVStreamEngine::canDispatchStreamLoad(
    const GemForgeDynInstInfo &dynInfo, ExecContext &xc) {

  // First create the memorized info.
  auto emplaceRet = this->seqNumToDynInfoMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(dynInfo.seqNum),
      std::forward_as_tuple());
  auto &userInfo = emplaceRet.first->second.userInfo;
  if (emplaceRet.second) {
    // First time. Translate the regionStreamId.
    auto regionStreamId = this->extractImm<uint64_t>(dynInfo.staticInst);
    auto streamId = this->lookupRegionStreamId(regionStreamId);
    userInfo.translatedUsedStreamIds.at(0) = streamId;
  }

  // TODO: Check LSQ entry if this is the first use of the element.
  return true;
}

void RISCVStreamEngine::dispatchStreamLoad(const GemForgeDynInstInfo &dynInfo,
                                           ExecContext &xc) {

  const auto &dynStreamInstInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  const auto &userInfo = dynStreamInstInfo.userInfo;
  std::vector<uint64_t> usedStreamIds{
      userInfo.translatedUsedStreamIds.at(0),
  };
  StreamEngine::StreamUserArgs args(dynInfo.seqNum, usedStreamIds);
  auto se = this->getStreamEngine(xc);
  se->dispatchStreamUser(args);
}

bool RISCVStreamEngine::canExecuteStreamLoad(const GemForgeDynInstInfo &dynInfo,
                                             ExecContext &xc) {
  const auto &dynStreamInstInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  const auto &userInfo = dynStreamInstInfo.userInfo;
  std::vector<uint64_t> usedStreamIds{
      userInfo.translatedUsedStreamIds.at(0),
  };
  StreamEngine::StreamUserArgs args(dynInfo.seqNum, usedStreamIds);
  auto se = this->getStreamEngine(xc);
  return se->areUsedStreamsReady(args);
}

void RISCVStreamEngine::executeStreamLoad(const GemForgeDynInstInfo &dynInfo,
                                          ExecContext &xc) {
  const auto &dynStreamInstInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  const auto &userInfo = dynStreamInstInfo.userInfo;
  std::vector<uint64_t> usedStreamIds{
      userInfo.translatedUsedStreamIds.at(0),
  };
  StreamEngine::StreamUserArgs::ValueVec values;
  values.reserve(usedStreamIds.size());
  StreamEngine::StreamUserArgs args(dynInfo.seqNum, usedStreamIds, &values);
  auto se = this->getStreamEngine(xc);
  se->executeStreamUser(args);
  auto loadedValue = *(reinterpret_cast<uint64_t *>(values.at(0).data()));
  hack("StreamLoad get value %llu.\n", loadedValue);
  xc.setIntRegOperand(dynInfo.staticInst, 0, loadedValue);
}

void RISCVStreamEngine::commitStreamLoad(const GemForgeDynInstInfo &dynInfo,
                                         ExecContext &xc) {
  const auto &dynStreamInstInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  const auto &userInfo = dynStreamInstInfo.userInfo;
  std::vector<uint64_t> usedStreamIds{
      userInfo.translatedUsedStreamIds.at(0),
  };
  StreamEngine::StreamUserArgs args(dynInfo.seqNum, usedStreamIds);
  auto se = this->getStreamEngine(xc);
  se->commitStreamUser(args);

  // Release the info.
  this->seqNumToDynInfoMap.erase(dynInfo.seqNum);
}

/********************************************************************************
 * StreamEngine Helpers.
 *******************************************************************************/

::StreamEngine *RISCVStreamEngine::getStreamEngine(ExecContext &xc) {
  return xc.tcBase()->getCpuPtr()->getAccelManager()->getStreamEngine();
}

template <typename T>
T RISCVStreamEngine::extractImm(const StaticInst *staticInst) const {
  auto immOp = dynamic_cast<const ImmOp<T> *>(staticInst);
  assert(immOp && "Invalid ImmOp.");
  return immOp->getImm();
}

const ::LLVM::TDG::StreamRegion &
RISCVStreamEngine::getStreamRegion(const std::string &path) const {
  if (this->memorizedStreamRegionMap.count(path) != 0) {
    return this->memorizedStreamRegionMap.at(path);
  }

  ProtoInputStream istream(path);
  auto &protobufRegion =
      this->memorizedStreamRegionMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(path),
                   std::forward_as_tuple())
          .first->second;
  if (!istream.read(protobufRegion)) {
    panic("Failed to read in the stream region from file %s.", path.c_str());
  }
  return protobufRegion;
}

void RISCVStreamEngine::insertRegionStreamIds(
    const ::LLVM::TDG::StreamRegion &region) {
  for (const auto &streamInfo : region.streams()) {
    auto streamId = streamInfo.id();
    auto regionStreamId = streamInfo.region_stream_id();
    assert(regionStreamId < 64 && "More than 64 streams in a region.");
    while (this->regionStreamIdTable.size() <= regionStreamId) {
      this->regionStreamIdTable.push_back(InvalidStreamId);
    }
    this->regionStreamIdTable.at(regionStreamId) = streamId;
  }
}

void RISCVStreamEngine::removeRegionStreamIds(
    const ::LLVM::TDG::StreamRegion &region) {
  for (const auto &streamInfo : region.streams()) {
    auto streamId = streamInfo.id();
    auto regionStreamId = streamInfo.region_stream_id();
    assert(this->regionStreamIdTable.size() > regionStreamId &&
           "Overflow RegionStreamId.");
    assert(this->regionStreamIdTable.at(regionStreamId) == streamId &&
           "RegionStreamId compromised.");
    this->regionStreamIdTable.at(regionStreamId) = InvalidStreamId;
  }
}

uint64_t RISCVStreamEngine::lookupRegionStreamId(int regionStreamId) {
  assert(this->regionStreamIdTable.size() > regionStreamId &&
         "Overflow RegionStreamId.");
  auto streamId = this->regionStreamIdTable.at(regionStreamId);
  assert(streamId != InvalidStreamId &&
         "RegionStreamId translated to InvalidStreamId.");
  return streamId;
}

RISCVStreamEngine::DynStreamInstInfo &
RISCVStreamEngine::createDynStreamInstInfo(uint64_t seqNum) {
  auto emplaceRet = this->seqNumToDynInfoMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(seqNum),
      std::forward_as_tuple());
  assert(emplaceRet.second && "StreamInstInfo already there.");
  return emplaceRet.first->second;
}

void RISCVStreamEngine::increamentStreamRegionInfoNumExecutedInsts(
    DynStreamRegionInfo &dynStreamRegionInfo, ExecContext &xc) {
  dynStreamRegionInfo.numExecutedInsts++;
  if (dynStreamRegionInfo.streamReadyDispatched &&
      dynStreamRegionInfo.numExecutedInsts ==
          dynStreamRegionInfo.numDispatchedInsts) {
    // We can notify the StreamEngine that StreamConfig can be executed,
    // including the InputMap.
    ::StreamEngine::StreamConfigArgs args(dynStreamRegionInfo.streamReadySeqNum,
                                          dynStreamRegionInfo.infoRelativePath,
                                          &dynStreamRegionInfo.inputMap);
    auto se = this->getStreamEngine(xc);
    se->executeStreamConfig(args);
  }
}

} // namespace RiscvISA