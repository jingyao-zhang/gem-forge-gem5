#include "riscv_stream_engine.hh"

#include "arch/riscv/insts/standard.hh"

#include "cpu/base.hh"
#include "cpu/exec_context.hh"
#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"
#include "proto/protoio.hh"

namespace RiscvISA {

constexpr uint64_t RISCVStreamEngine::InvalidStreamId;
constexpr int RISCVStreamEngine::DynStreamInstInfo::MaxStreamIds;

/********************************************************************************
 * StreamConfig Handlers.
 *******************************************************************************/

bool RISCVStreamEngine::canDispatchStreamConfig(
    const GemForgeDynInstInfo &dynInfo, ExecContext &xc) {
  auto se = this->getStreamEngine(xc);
  auto rs1 = xc.readIntRegOperand(dynInfo.staticInst, 0);
  auto cpuDelegator = xc.tcBase()->getCpuPtr()->getCPUDelegator();
  assert(cpuDelegator && "Failed to find the CPUDelegator.");
  auto infoRelativePath = cpuDelegator->readStringFromMem(rs1);
  ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum, infoRelativePath);
  return se->canStreamConfig(args);
}

void RISCVStreamEngine::dispatchStreamConfig(const GemForgeDynInstInfo &dynInfo,
                                             ExecContext &xc) {
  auto se = this->getStreamEngine(xc);
  auto rs1 = xc.readIntRegOperand(dynInfo.staticInst, 0);
  auto cpuDelegator = xc.tcBase()->getCpuPtr()->getCPUDelegator();
  assert(cpuDelegator && "Failed to find the CPUDelegator.");
  auto infoRelativePath = cpuDelegator->readStringFromMem(rs1);

  auto infoFullPath =
      cpuDelegator->getTraceExtraFolder() + "/" + infoRelativePath;
  const auto &info = this->getStreamRegion(infoFullPath);
  this->insertRegionStreamIds(info);

  ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum, infoRelativePath);
  se->dispatchStreamConfig(args);
}

void RISCVStreamEngine::executeStreamConfig(const GemForgeDynInstInfo &dynInfo,
                                            ExecContext &xc) {
  auto se = this->getStreamEngine(xc);
  auto rs1 = xc.readIntRegOperand(dynInfo.staticInst, 0);
  auto cpuDelegator = xc.tcBase()->getCpuPtr()->getCPUDelegator();
  assert(cpuDelegator && "Failed to find the CPUDelegator.");
  auto infoRelativePath = cpuDelegator->readStringFromMem(rs1);
  ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum, infoRelativePath);
  se->executeStreamConfig(args);
}

void RISCVStreamEngine::commitStreamConfig(const GemForgeDynInstInfo &dynInfo,
                                           ExecContext &xc) {
  auto se = this->getStreamEngine(xc);
  auto rs1 = xc.readIntRegOperand(dynInfo.staticInst, 0);
  auto cpuDelegator = xc.tcBase()->getCpuPtr()->getCPUDelegator();
  assert(cpuDelegator && "Failed to find the CPUDelegator.");
  auto infoRelativePath = cpuDelegator->readStringFromMem(rs1);
  ::StreamEngine::StreamConfigArgs args(dynInfo.seqNum, infoRelativePath);
  se->commitStreamConfig(args);
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
  if (emplaceRet.second) {
    // First time. Translate the regionStreamId.
    auto regionStreamId = this->extractImm<uint64_t>(dynInfo.staticInst);
    auto streamId = this->lookupRegionStreamId(regionStreamId);
    emplaceRet.first->second.translatedStreamIds.at(0) = streamId;
  }

  auto streamId = emplaceRet.first->second.translatedStreamIds.at(0);

  auto se = this->getStreamEngine(xc);
  return se->canStreamStep(streamId);
}

void RISCVStreamEngine::dispatchStreamStep(const GemForgeDynInstInfo &dynInfo,
                                           ExecContext &xc) {

  const auto &dynStreamInstInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  auto streamId = dynStreamInstInfo.translatedStreamIds.at(0);
  auto se = this->getStreamEngine(xc);
  se->dispatchStreamStep(streamId);
}

void RISCVStreamEngine::executeStreamStep(const GemForgeDynInstInfo &dynInfo,
                                          ExecContext &xc) {}

void RISCVStreamEngine::commitStreamStep(const GemForgeDynInstInfo &dynInfo,
                                         ExecContext &xc) {
  const auto &dynStreamInstInfo = this->seqNumToDynInfoMap.at(dynInfo.seqNum);
  auto streamId = dynStreamInstInfo.translatedStreamIds.at(0);
  auto se = this->getStreamEngine(xc);
  se->commitStreamStep(streamId);

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

} // namespace RiscvISA