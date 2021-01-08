#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_COMPUTE_ENGINE_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_COMPUTE_ENGINE_HH__

#include "stream_engine.hh"

/**
 * A tiny structure that models an ideal computation engine with
 * some width and variable latency. It assumes to be pipelined.
 */

class StreamComputeEngine {
public:
  StreamComputeEngine(StreamEngine *_se, StreamEngine::Params *_params);

  void pushReadyComputation(StreamElement *element, StreamValue result,
                            Cycles latency);
  void startComputation();
  void completeComputation();
  void discardComputation(StreamElement *element);

private:
  struct Computation {
    StreamElement *element;
    StreamValue result;
    const Cycles latency;
    Cycles readyCycle = Cycles(0);
    Computation(StreamElement *_element, StreamValue _result, Cycles _latency)
        : element(_element), result(std::move(_result)), latency(_latency) {}
  };
  using ComputationPtr = std::unique_ptr<Computation>;

  StreamEngine *se;
  const int computeWidth;
  const bool forceZeroLatency;

  std::list<ComputationPtr> readyComputations;
  std::list<ComputationPtr> inflyComputations;

  void pushInflyComputation(ComputationPtr computation);
};

#endif