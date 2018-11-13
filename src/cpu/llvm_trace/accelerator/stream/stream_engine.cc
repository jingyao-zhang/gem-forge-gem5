#include "stream_engine.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"

#include "base/misc.hh"
#include "base/trace.hh"
#include "debug/StreamEngine.hh"

StreamEngine::StreamEngine() : TDGAccelerator(), isOracle(false) {}

StreamEngine::~StreamEngine() {
  // Clear all the allocated streams.
  for (auto &streamIdStreamPair : this->streamMap) {
    delete streamIdStreamPair.second;
    streamIdStreamPair.second = nullptr;
  }
  this->streamMap.clear();
}

void StreamEngine::handshake(LLVMTraceCPU *_cpu,
                             TDGAcceleratorManager *_manager) {
  TDGAccelerator::handshake(_cpu, _manager);

  auto cpuParams = dynamic_cast<const LLVMTraceCPUParams *>(_cpu->params());
  this->setIsOracle(cpuParams->streamEngineIsOracle);
  this->maxRunAHeadLength = cpuParams->streamEngineMaxRunAHeadLength;
  this->throttling = cpuParams->streamEngineThrottling;
  this->enableCoalesce = cpuParams->streamEngineEnableCoalesce;
}

void StreamEngine::regStats() {
  this->numConfigured.name(this->manager->name() + ".stream.numConfigured")
      .desc("Number of streams configured.")
      .prereq(this->numConfigured);
  this->numStepped.name(this->manager->name() + ".stream.numStepped")
      .desc("Number of streams stepped.")
      .prereq(this->numStepped);
  this->numStreamMemRequests
      .name(this->manager->name() + ".stream.numStreamMemRequests")
      .desc("Number of stream memory requests.")
      .prereq(this->numStreamMemRequests);
  this->numElements.name(this->manager->name() + ".stream.numElements")
      .desc("Number of stream elements created.")
      .prereq(this->numElements);
  this->numElementsUsed.name(this->manager->name() + ".stream.numElementsUsed")
      .desc("Number of stream elements used.")
      .prereq(this->numElementsUsed);
  this->entryWaitCycles.name(this->manager->name() + ".stream.entryWaitCycles")
      .desc("Number of cycles from first checked ifReady to ready.")
      .prereq(this->entryWaitCycles);
  this->numMemElements.name(this->manager->name() + ".stream.numMemElements")
      .desc("Number of mem stream elements created.")
      .prereq(this->numMemElements);
  this->numMemElementsFetched
      .name(this->manager->name() + ".stream.numMemElementsFetched")
      .desc("Number of mem stream elements fetched from cache.")
      .prereq(this->numMemElementsFetched);
  this->numMemElementsUsed
      .name(this->manager->name() + ".stream.numMemElementsUsed")
      .desc("Number of mem stream elements used.")
      .prereq(this->numMemElementsUsed);
  this->memEntryWaitCycles
      .name(this->manager->name() + ".stream.memEntryWaitCycles")
      .desc("Number of cycles of a mem entry from first checked ifReady to "
            "ready.")
      .prereq(this->memEntryWaitCycles);

  this->numTotalAliveElements.init(0, 1000, 50)
      .name(this->manager->name() + ".stream.numTotalAliveElements")
      .desc("Number of alive stream elements in each cycle.")
      .flags(Stats::pdf);
  this->numTotalAliveCacheBlocks.init(0, 1000, 50)
      .name(this->manager->name() + ".stream.numTotalAliveCacheBlocks")
      .desc("Number of alive cache blocks in each cycle.")
      .flags(Stats::pdf);
  this->numRunAHeadLengthDist.init(0, 15, 1)
      .name(this->manager->name() + ".stream.numRunAHeadLengthDist")
      .desc("Number of run ahead length for streams.")
      .flags(Stats::pdf);
  this->numTotalAliveMemStreams.init(0, 15, 1)
      .name(this->manager->name() + ".stream.numTotalAliveMemStreams")
      .desc("Number of alive memory stream.")
      .flags(Stats::pdf);
}

bool StreamEngine::handle(LLVMDynamicInst *inst) {
  if (auto configInst = dynamic_cast<StreamConfigInst *>(inst)) {
    this->numConfigured++;
    auto S = this->getOrInitializeStream(configInst->getTDG().stream_config());
    S->configure(configInst);
    configInst->markFinished();
    return true;
  }
  if (auto stepInst = dynamic_cast<StreamStepInst *>(inst)) {
    this->numStepped++;
    auto stream =
        this->getStreamNullable(stepInst->getTDG().stream_step().stream_id());
    auto stepSeqNum = stepInst->getSeqNum();
    if (stream != nullptr && (!stream->isBeforeFirstConfigInst(stepSeqNum))) {
      stream->step(stepInst);
    }
    stepInst->markFinished();
    return true;
  }
  if (auto storeInst = dynamic_cast<StreamStoreInst *>(inst)) {
    auto stream =
        this->getStreamNullable(storeInst->getTDG().stream_store().stream_id());
    auto storeSeqNum = storeInst->getSeqNum();
    if (stream != nullptr && (!stream->isBeforeFirstConfigInst(storeSeqNum))) {
      stream->store(storeInst);
    }
    storeInst->markFinished();
    return true;
  }
  if (auto endInst = dynamic_cast<StreamEndInst *>(inst)) {
    auto stream =
        this->getStreamNullable(endInst->getTDG().stream_end().stream_id());
    auto endSeqNum = endInst->getSeqNum();
    if (stream != nullptr && (!stream->isBeforeFirstConfigInst(endSeqNum))) {
      stream->end(endInst);
    }
    endInst->markFinished();
    return true;
  }
  return false;
}

bool StreamEngine::isStreamReady(uint64_t streamId,
                                 const LLVMDynamicInst *user) const {
  auto stream = this->getStreamNullable(streamId);
  if (stream == nullptr) {
    // This is possible in partial datagraph that contains an incomplete loop.
    // For this rare case, we just assume the stream is ready.
    return true;
  }
  return stream->isReady(user);
}

void StreamEngine::useStream(uint64_t streamId, const LLVMDynamicInst *user) {
  auto stream = this->getStreamNullable(streamId);
  if (stream == nullptr) {
    // This is possible in partial datagraph that contains an incomplete loop.
    // For this rare case, we just assume the stream is ready.
    return;
  }
  return stream->use(user);
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

void StreamEngine::commitStreamConfigure(StreamConfigInst *inst) {
  auto streamId = inst->getTDG().stream_config().stream_id();
  auto seqNum = inst->getSeqNum();
  auto stream = this->getStreamNullable(streamId);
  if (stream == nullptr || stream->isBeforeFirstConfigInst(seqNum)) {
    // This is possible in partial datagraph that contains an incomplete loop.
    return;
  }
  stream->commitConfigure(inst);
}

void StreamEngine::commitStreamStep(StreamStepInst *inst) {
  auto streamId = inst->getTDG().stream_step().stream_id();
  auto seqNum = inst->getSeqNum();
  auto stream = this->getStreamNullable(streamId);
  if (stream == nullptr || stream->isBeforeFirstConfigInst(seqNum)) {
    // This is possible in partial datagraph that contains an incomplete loop.
    return;
  }
  stream->commitStep(inst);
}

void StreamEngine::commitStreamStore(StreamStoreInst *inst) {
  auto streamId = inst->getTDG().stream_store().stream_id();
  auto seqNum = inst->getSeqNum();
  auto stream = this->getStreamNullable(streamId);
  if (stream == nullptr || stream->isBeforeFirstConfigInst(seqNum)) {
    // This is possible in partial datagraph that contains an incomplete loop.
    // if (storeSeqNum == 5742) {
    //   panic("chhhh %d.", stream->getFirstConfigSeqNum());
    // }
    return;
  }
  // if (storeSeqNum == 5742) {
  //   panic("christ jesus.");
  // }
  stream->commitStore(inst);
}

void StreamEngine::commitStreamEnd(StreamEndInst *inst) {
  auto streamId = inst->getTDG().stream_end().stream_id();
  auto seqNum = inst->getSeqNum();
  auto stream = this->getStreamNullable(streamId);
  if (stream == nullptr || stream->isBeforeFirstConfigInst(seqNum)) {
    return;
  }
  stream->commitEnd(inst);
}

Stream *StreamEngine::getOrInitializeStream(
    const LLVM::TDG::TDGInstruction_StreamConfigExtra &configInst) {
  const auto &streamId = configInst.stream_id();
  auto iter = this->streamMap.find(streamId);
  if (iter == this->streamMap.end()) {
    Stream *NewStream =
        new SingleStream(configInst, cpu, this, this->isOracle,
                         this->maxRunAHeadLength, this->throttling);

    iter =
        this->streamMap
            .emplace(std::piecewise_construct, std::forward_as_tuple(streamId),
                     std::forward_as_tuple(NewStream))
            .first;
  }
  return iter->second;
}

const Stream *StreamEngine::getStreamNullable(uint64_t streamId) const {
  auto iter = this->streamMap.find(streamId);
  if (iter == this->streamMap.end()) {
    return nullptr;
  }
  return iter->second;
}

Stream *StreamEngine::getStreamNullable(uint64_t streamId) {
  auto iter = this->streamMap.find(streamId);
  if (iter == this->streamMap.end()) {
    return nullptr;
  }
  return iter->second;
}

void StreamEngine::tick() {
  if (curTick() % 10000 == 0) {
    this->updateAliveStatistics();
  }
}

void StreamEngine::updateAliveStatistics() {
  int totalAliveElements = 0;
  int totalAliveMemStreams = 0;
  std::unordered_set<Addr> totalAliveCacheBlocks;
  this->numRunAHeadLengthDist.reset();
  for (const auto &streamPair : this->streamMap) {
    const auto &stream = streamPair.second;
    if (stream->isMemStream()) {
      this->numRunAHeadLengthDist.sample(stream->getRunAheadLength());
    }
    if (!stream->isConfigured()) {
      continue;
    }
    if (stream->isMemStream()) {
      totalAliveElements += stream->getAliveElements();
      totalAliveMemStreams++;
      for (const auto &cacheBlockAddrPair : stream->getAliveCacheBlocks()) {
        totalAliveCacheBlocks.insert(cacheBlockAddrPair.first);
      }
    }
  }
  this->numTotalAliveElements.sample(totalAliveElements);
  this->numTotalAliveCacheBlocks.sample(totalAliveCacheBlocks.size());
  this->numTotalAliveMemStreams.sample(totalAliveMemStreams);
}
