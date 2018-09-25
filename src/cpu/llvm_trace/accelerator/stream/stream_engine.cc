#include "stream_engine.hh"

#include "base/misc.hh"
#include "base/trace.hh"
#include "debug/StreamEngine.hh"

StreamEngine::StreamEngine() : TDGAccelerator() {}

StreamEngine::~StreamEngine() {}

void StreamEngine::regStats() {
  this->numConfigured.name(this->manager->name() + ".stream.numConfigured")
      .desc("Number of streams configured.")
      .prereq(this->numConfigured);
  this->numStepped.name(this->manager->name() + ".stream.numStepped")
      .desc("Number of streams stepped.")
      .prereq(this->numStepped);
}

bool StreamEngine::handle(LLVMDynamicInst *inst) {
  if (auto configInst = dynamic_cast<StreamConfigInst *>(inst)) {
    this->numConfigured++;
    auto S = this->getOrInitializeStream(configInst->getTDG().stream_config());
    S->configure(configInst->getSeqNum());
    configInst->markFinished();
    return true;
  }
  if (auto stepInst = dynamic_cast<StreamStepInst *>(inst)) {
    this->numStepped++;
    auto stream =
        this->getStreamNullable(stepInst->getTDG().stream_step().stream_id());
    auto stepSeqNum = stepInst->getSeqNum();
    if (stream != nullptr && stream->getConfigSeqNum() < stepSeqNum) {
      stream->step(stepSeqNum);
    }
    stepInst->markFinished();
    return true;
  }
  if (auto storeInst = dynamic_cast<StreamStoreInst *>(inst)) {
    auto stream =
        this->getStreamNullable(storeInst->getTDG().stream_store().stream_id());
    auto storeSeqNum = storeInst->getSeqNum();
    if (stream != nullptr && stream->getConfigSeqNum() < storeSeqNum) {
      stream->store(storeSeqNum);
    }
    storeInst->markFinished();
    return true;
  }
  return false;
}

bool StreamEngine::isStreamReady(uint64_t streamId, uint64_t userSeqNum) const {
  auto stream = this->getStreamNullable(streamId);
  if (stream == nullptr) {
    // This is possible in partial datagraph that contains an incomplete loop.
    // For this rare case, we just assume the stream is ready.
    return true;
  }
  return stream->isReady(userSeqNum);
}

bool StreamEngine::canStreamStep(uint64_t streamId) const {
  auto stream = this->getStreamNullable(streamId);
  if (stream == nullptr) {
    // This is possible in partial datagraph that contains an incomplete loop.
    // For this rare case, we just assume the stream is ready.
    return true;
  }
  return stream->canStep();
}

void StreamEngine::commitStreamStep(uint64_t streamId, uint64_t stepSeqNum) {
  auto stream = this->getStreamNullable(streamId);
  if (stream == nullptr || stream->getConfigSeqNum() > stepSeqNum) {
    // This is possible in partial datagraph that contains an incomplete loop.
    return;
  }
  stream->commitStep(stepSeqNum);
}

void StreamEngine::commitStreamStore(uint64_t streamId, uint64_t storeSeqNum) {
  auto stream = this->getStreamNullable(streamId);
  if (stream == nullptr || stream->getConfigSeqNum() > storeSeqNum) {
    // This is possible in partial datagraph that contains an incomplete loop.
    return;
  }
  stream->commitStore(storeSeqNum);
}

Stream *StreamEngine::getOrInitializeStream(
    const LLVM::TDG::TDGInstruction_StreamConfigExtra &configInst) {
  const auto &streamId = configInst.stream_id();
  auto iter = this->streamMap.find(streamId);
  if (iter == this->streamMap.end()) {
    iter =
        this->streamMap
            .emplace(std::piecewise_construct, std::forward_as_tuple(streamId),
                     std::forward_as_tuple(configInst, cpu, this))
            .first;
  }
  return &(iter->second);
}

const Stream *StreamEngine::getStreamNullable(uint64_t streamId) const {
  auto iter = this->streamMap.find(streamId);
  if (iter == this->streamMap.end()) {
    return nullptr;
  }
  return &(iter->second);
}

Stream *StreamEngine::getStreamNullable(uint64_t streamId) {
  auto iter = this->streamMap.find(streamId);
  if (iter == this->streamMap.end()) {
    return nullptr;
  }
  return &(iter->second);
}

void StreamEngine::tick() {}
