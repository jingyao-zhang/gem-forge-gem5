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
}

bool StreamEngine::handle(LLVMDynamicInst *inst) {
  if (auto configInst = dynamic_cast<StreamConfigInst *>(inst)) {
    this->numConfigured++;
    this->initializeStreamForFirstTime(configInst->getTDG());
    configInst->markFinished();
    return true;
  }
  if (auto stepInst = dynamic_cast<StreamStepInst *>(inst)) {
    stepInst->markFinished();
    return true;
  }
  if (auto storeInst = dynamic_cast<StreamStoreInst *>(inst)) {
    storeInst->markFinished();
    return true;
  }
  return false;
}

void StreamEngine::initializeStreamForFirstTime(
    const LLVM::TDG::TDGInstruction &configInst) {
  if (!configInst.has_stream_config()) {
    panic("initialize stream for non stream-config instruction.");
  }
  const auto &streamId = configInst.stream_config().stream_id();
  auto iter = this->streamMap.find(streamId);
  if (iter == this->streamMap.end()) {
    this->streamMap.emplace(std::piecewise_construct,
                            std::forward_as_tuple(streamId),
                            std::forward_as_tuple(configInst));
  }
}

void StreamEngine::tick() {}
