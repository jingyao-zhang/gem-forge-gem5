#include "stream_ndc_controller.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamEngine, format, ##args)

#include "debug/StreamEngine.hh"
#define DEBUG_TYPE StreamEngine
#include "stream_log.hh"

StreamNDCController::StreamNDCController(StreamEngine *_se) : se(_se) {}

void StreamNDCController::offloadStreams(
    const StreamConfigArgs &args, const ::LLVM::TDG::StreamRegion &region,
    DynStreamList &dynStreams) {

  /**
   * We first offload single operand compute streams.
   */
  for (auto dynS : dynStreams) {
    auto S = dynS->stream;
    if (S->isAtomicComputeStream()) {
      dynS->offloadedAsNDC = true;
      S->statistic.numFineGrainedOffloaded++;
    }
  }
}
