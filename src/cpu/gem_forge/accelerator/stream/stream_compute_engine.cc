#include "stream_compute_engine.hh"

#include "base/trace.hh"
#include "debug/StreamEngine.hh"

#define DEBUG_TYPE StreamEngine
#include "stream_log.hh"

StreamComputeEngine::StreamComputeEngine(StreamEngine *_se,
                                         StreamEngine::Params *_params)
    : se(_se), computeWidth(_params->computeWidth),
      forceZeroLatency(_params->enableZeroComputeLatency) {}

void StreamComputeEngine::pushReadyComputation(StreamElement *element,
                                               StreamValue result,
                                               Cycles latency) {
  if (this->forceZeroLatency) {
    latency = Cycles(0);
  }
  auto computation =
      m5::make_unique<Computation>(element, std::move(result), latency);
  this->readyComputations.emplace_back(std::move(computation));
  element->scheduledComputation = true;
}

void StreamComputeEngine::startComputation() {

  int startedComputation = 0;
  while (startedComputation < this->computeWidth &&
         !this->readyComputations.empty()) {
    auto computation = std::move(this->readyComputations.front());

    S_ELEMENT_DPRINTF(computation->element,
                      "Start computation. Charge Latency %llu.\n",
                      computation->latency);
    this->pushInflyComputation(std::move(computation));

    this->readyComputations.pop_front();
    startedComputation++;
  }
}

void StreamComputeEngine::completeComputation() {

  // We don't charge complete width.
  auto curCycle = this->se->curCycle();
  while (!this->inflyComputations.empty()) {
    auto &computation = this->inflyComputations.front();
    auto element = computation->element;
    if (computation->readyCycle > curCycle) {
      S_ELEMENT_DPRINTF(
          element,
          "Cannot complete computation, readyCycle %llu, curCycle %llu.\n",
          computation->readyCycle, curCycle);
      break;
    }
    S_ELEMENT_DPRINTF(element, "Complete computation.\n");
    element->receiveComputeResult(computation->result);
    element->scheduledComputation = false;
    this->inflyComputations.pop_front();
  }
}

void StreamComputeEngine::pushInflyComputation(ComputationPtr computation) {

  assert(this->inflyComputations.size() < 100 && "Too many infly results.");
  assert(computation->latency < 100 && "Latency too long.");

  computation->readyCycle = this->se->curCycle() + computation->latency;
  for (auto iter = this->inflyComputations.rbegin(),
            end = this->inflyComputations.rend();
       iter != end; ++iter) {
    if ((*iter)->readyCycle <= computation->readyCycle) {
      this->inflyComputations.emplace(iter.base(), std::move(computation));
      return;
    }
  }
  this->inflyComputations.emplace_front(std::move(computation));
}

void StreamComputeEngine::discardComputation(StreamElement *element) {
  if (!element->scheduledComputation) {
    S_ELEMENT_PANIC(element, "No scheduled computation to be discarded.");
  }
  for (auto iter = this->inflyComputations.begin(),
            end = this->inflyComputations.end();
       iter != end; ++iter) {
    auto &computation = *iter;
    if (computation->element == element) {
      element->scheduledComputation = false;
      this->inflyComputations.erase(iter);
      return;
    }
  }
  for (auto iter = this->readyComputations.begin(),
            end = this->readyComputations.end();
       iter != end; ++iter) {
    auto &computation = *iter;
    if (computation->element == element) {
      element->scheduledComputation = false;
      this->readyComputations.erase(iter);
      return;
    }
  }
  S_ELEMENT_PANIC(element, "Failed to find the scheduled computation.");
}