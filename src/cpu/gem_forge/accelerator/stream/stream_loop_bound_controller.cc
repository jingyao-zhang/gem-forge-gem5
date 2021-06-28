#include "stream_loop_bound_controller.hh"

#include "base/trace.hh"
#include "debug/StreamLoopBound.hh"

#define DEBUG_TYPE StreamLoopBound
#include "stream_log.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamLoopBound, format, ##args)

StreamLoopBoundController::StreamLoopBoundController(StreamEngine *_se)
    : se(_se) {}

StreamLoopBoundController::~StreamLoopBoundController() {}

void StreamLoopBoundController::initializeLoopBound(
    const ::LLVM::TDG::StreamRegion &region) {
  if (!region.is_loop_bound()) {
    return;
  }

  const auto &boundFuncInfo = region.loop_bound_func();
  auto boundFunc = std::make_shared<TheISA::ExecFunc>(
      se->getCPUDelegator()->getSingleThreadContext(), boundFuncInfo);
  const bool boundRet = region.loop_bound_ret();

  this->staticBoundMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(region.region()),
      std::forward_as_tuple(region, boundFunc, boundRet));

  SE_DPRINTF("[LoopBound] Initialized StaticLoopBound for region %s.\n",
             region.region());
  auto &staticBound = this->staticBoundMap.at(region.region());

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

void StreamLoopBoundController::dispatchStreamConfig(const ConfigArgs &args) {}

void StreamLoopBoundController::executeStreamConfig(const ConfigArgs &args) {}

void StreamLoopBoundController::rewindStreamConfig(const ConfigArgs &args) {}

void StreamLoopBoundController::commitStreamEnd(const EndArgs &args) {}