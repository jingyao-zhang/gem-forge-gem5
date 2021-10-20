#include "StreamReuseBuffer.hh"

#include "debug/RemoteStreamReuse.hh"
#define DEBUG_TYPE RemoteStreamReuse
#include "../stream_log.hh"

ReuseBuffer::ReuseBuffer(int _maxNumLines) : maxNumLines(_maxNumLines) {}

void ReuseBuffer::addLine(Addr paddrLine, const DataBlock &dataBlock) {
  if (this->cachedLines.size() == this->maxNumLines) {
    const auto &oldestLine = this->cachedLines.front();
    this->paddrLineToCachedIterMap.erase(oldestLine.paddrLine);
    this->cachedLines.pop_front();
  }
  auto iter =
      this->cachedLines.emplace(this->cachedLines.end(), paddrLine, dataBlock);
  this->paddrLineToCachedIterMap.emplace(paddrLine, iter);
}

bool ReuseBuffer::contains(Addr paddrLine) {
  return this->paddrLineToCachedIterMap.count(paddrLine);
}

const DataBlock &ReuseBuffer::reuse(Addr paddrLine) {
  assert(this->contains(paddrLine) && "Line is not cached for reuse.");
  auto cachedIter = this->paddrLineToCachedIterMap.at(paddrLine);
  // this->cachedLines.splice(this->cachedLines.end(), this->cachedLines,
  //                          cachedIter);
  return cachedIter->dataBlock;
}

StreamReuseBuffer::StreamReuseBuffer(MachineID _machineId, int _maxNumLines,
                                     bool _perCoreMode)
    : machineId(_machineId), maxNumLines(_maxNumLines),
      perCoreMode(_perCoreMode) {
  auto type = this->machineId.type;
  switch (type) {
  case MachineType::MachineType_L2Cache:
    this->machineTypeStr = "LLC_Reuse";
    break;
  case MachineType::MachineType_Directory:
    this->machineTypeStr = "MEM_Reuse";
    break;
  default:
    this->machineTypeStr = "XXX_Reuse";
    break;
  }
}

int StreamReuseBuffer::curRemoteBank() const { return this->machineId.num; }

MachineType StreamReuseBuffer::myMachineType() const {
  return this->machineId.getType();
}

const char *StreamReuseBuffer::curRemoteMachineType() const {
  return this->machineTypeStr;
}

void StreamReuseBuffer::addLine(const DynamicStreamSliceId &sliceId,
                                Addr paddrLine, const DataBlock &dataBlock) {
  if (this->maxNumLines == 0) {
    return;
  }
  LLC_SLICE_DPRINTF(sliceId, "Added %#x.\n", paddrLine);
  this->getReuseBuffer(sliceId.getDynStreamId())->addLine(paddrLine, dataBlock);
}

bool StreamReuseBuffer::contains(const DynamicStreamSliceId &reuseSliceId,
                                 Addr paddrLine) {
  if (this->maxNumLines == 0) {
    return false;
  }
  bool containing =
      this->getReuseBuffer(reuseSliceId.getDynStreamId())->contains(paddrLine);
  LLC_SLICE_DPRINTF(reuseSliceId, "%s %#x.\n",
                    (containing ? "Containing" : "Missing"), paddrLine);
  return containing;
}

const DataBlock &
StreamReuseBuffer::reuse(const DynamicStreamSliceId &reuseSliceId,
                         Addr paddrLine) {
  assert(this->maxNumLines > 0 && "StreamReuseBuffer is disabled.");
  LLC_SLICE_DPRINTF(reuseSliceId, "Reused %#x.\n", paddrLine);
  return this->getReuseBuffer(reuseSliceId.getDynStreamId())->reuse(paddrLine);
}

int StreamReuseBuffer::getCoreId(const DynamicStreamId &dynSId) const {
  if (this->perCoreMode) {
    return dynSId.coreId;
  } else {
    // All cores share the same reuse buffer.
    return 0;
  }
}

StreamReuseBuffer::ReuseBufferPtr &
StreamReuseBuffer::getReuseBuffer(const DynamicStreamId &dynSId) {
  auto coreId = this->getCoreId(dynSId);
  if (!(coreId >= 0 && coreId < 64)) {
    DYN_S_PANIC(dynSId, "Invalid CoreId %d.");
  }
  while (this->coreToReuseBuffers.size() <= coreId) {
    this->coreToReuseBuffers.push_back(
        std::make_shared<ReuseBuffer>(this->maxNumLines));
  }
  return this->coreToReuseBuffers.at(coreId);
}

bool StreamReuseBuffer::shouldCacheStream(Stream *S,
                                          const DynamicStreamId &dynSId) const {
  /**
   * We cache if we are enabled and the Stream has more than one AliasedStream.
   */
  if (this->maxNumLines == 0) {
    return false;
  }
  if (!S->aliasBaseStream) {
    return false;
  }
  const auto &aliasedStreams = S->aliasBaseStream->aliasedStreams;
  if (aliasedStreams.size() <= 1) {
    return false;
  }
  if (S != aliasedStreams.back()) {
    return false;
  }
  return true;
}

bool StreamReuseBuffer::shouldCheckReuse(Stream *S,
                                         const DynamicStreamId &dynSId) const {
  /**
   * We cache if we are enabled and the Stream has more than one AliasedStream.
   */
  if (this->maxNumLines == 0) {
    return false;
  }
  if (!S->aliasBaseStream) {
    return false;
  }
  const auto &aliasedStreams = S->aliasBaseStream->aliasedStreams;
  if (aliasedStreams.size() <= 1) {
    return false;
  }
  if (S == aliasedStreams.back()) {
    return false;
  }
  return true;
}