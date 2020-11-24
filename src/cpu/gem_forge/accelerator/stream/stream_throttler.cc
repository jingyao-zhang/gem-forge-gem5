#include "stream_throttler.hh"

#include "debug/StreamThrottle.hh"

#define ST_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]-Throttle: " format, this->se->cpuDelegator->cpuId(),      \
          ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamThrottle, format, ##args)

#define DEBUG_TYPE StreamThrottle
#include "stream_log.hh"

StreamThrottler::StreamThrottler(const std::string &_strategy,
                                 StreamEngine *_se)
    : se(_se) {
  if (_strategy == "static") {
    this->strategy = StrategyE::STATIC;
  } else if (_strategy == "dynamic") {
    this->strategy = StrategyE::DYNAMIC;
  } else {
    this->strategy = StrategyE::GLOBAL;
  }
}

const std::string StreamThrottler::name() const { return this->se->name(); }

/********************************************************************
 * Check if we actually want to throttle.
 *******************************************************************/

void StreamThrottler::throttleStream(Stream *S, StreamElement *element) {
  if (this->strategy == StrategyE::STATIC) {
    // Static means no throttling.
    return;
  }
  if (S->isStoreStream()) {
    // No need to throttle for store stream.
    return;
  }
  if (element->FIFOIdx.entryIdx < S->maxSize) {
    // Do not throttle for the first elements.
    return;
  }
  if (element->valueReadyCycle == 0 || element->firstCheckCycle == 0) {
    // No valid cycle record, do nothing.
    return;
  }
  if (element->valueReadyCycle < element->firstCheckCycle) {
    // The element is ready earlier than user, do nothing.
    return;
  }
  // This is a late fetch, increase the counter.
  S->lateFetchCount++;
  S_ELEMENT_DPRINTF_(StreamThrottle, element, "[Throttle] LateCount %d.\n",
                     S->lateFetchCount);
  if (S->lateFetchCount == 10) {
    // We have reached the threshold to allow the stream to run further
    // ahead.
    auto oldRunAheadSize = S->maxSize;
    /**
     * Get the step root stream.
     * Sometimes, it is possible that stepRootStream is nullptr,
     * which means that this is a constant stream.
     * We do not throttle in this case.
     */
    auto stepRootStream = S->stepRootStream;
    if (stepRootStream != nullptr) {
      const auto &streamList = this->se->getStepStreamList(stepRootStream);
      if (this->strategy == StrategyE::DYNAMIC) {
        // All streams with the same stepRootStream must have the same run
        // ahead length.
        auto totalRunAheadLength = this->se->getTotalRunAheadLength();
        // Only increase the run ahead length if the totalRunAheadLength is
        // within the 90% of the total FIFO entries. Need better solution
        // here.
        const auto incrementStep = 2;
        if (static_cast<float>(totalRunAheadLength) <
            0.9f * static_cast<float>(this->se->FIFOArray.size())) {
          for (auto stepS : streamList) {
            // Increase the run ahead length by step.
            stepS->maxSize += incrementStep;
          }
          assert(S->maxSize == oldRunAheadSize + 2 &&
                 "RunAheadLength is not increased.");
        }
      } else if (this->strategy == StrategyE::GLOBAL) {
        this->doThrottling(S, element);
      }
      // No matter what, just clear the lateFetchCount in the whole step
      // group.
      for (auto stepS : streamList) {
        stepS->lateFetchCount = 0;
      }
    } else {
      // Otherwise, just clear my self.
      S->lateFetchCount = 0;
    }
  }
}

/********************************************************************
 * Perform the actual throttling.
 *
 * When we trying to throttle a stream, the main problem is to avoid
 * deadlock, as we do not reclaim stream element once it is allocated until
 * it is stepped.
 *
 * To avoid deadlock, we leverage the information of total alive streams
 * that can coexist with the current stream, and assign InitMaxSize number
 * of elements to these streams, which is called BasicEntries.
 * * BasicEntries = TotalAliveStreams * InitMaxSize.
 *
 * Then we want to know how many of these BasicEntries is already assigned
 * to streams. This number is called AssignedBasicEntries.
 * * AssignedBasicEntries = CurrentAliveStreams * InitMaxSize.
 *
 * We also want to know the number of AssignedEntries and UnAssignedEntries.
 * * AssignedEntries = Sum(MaxSize, CurrentAliveStreams).
 * * UnAssignedEntries = FIFOSize - AssignedEntries.
 *
 * The available pool for throttling is:
 * * AvailableEntries = \
 * *   UnAssignedEntries - (BasicEntries - AssignedBasicEntries).
 *
 * Also we enforce an upper bound on the entries:
 * * UpperBoundEntries = \
 * *   (FIFOSize - BasicEntries) / StepGroupSize + InitMaxSize.
 *
 * As we are throttling streams altogether with the same stepRoot, the
 * condition is:
 * * AvailableEntries >= IncrementSize * StepGroupSize.
 * * CurrentMaxSize + IncrementSize <= UpperBoundEntries
 *
 ********************************************************************/

void StreamThrottler::doThrottling(Stream *S, StreamElement *element) {
  auto stepRootStream = S->stepRootStream;
  assert(stepRootStream != nullptr &&
         "Do not make sense to throttle for a constant stream.");
  const auto &streamList = this->se->getStepStreamList(stepRootStream);

  S_ELEMENT_DPRINTF_(StreamThrottle, element, "[Throttle] Do throttling.\n");

  // * AssignedEntries.
  auto currentAliveStreams = 0;
  auto assignedEntries = 0;
  for (const auto &IdStream : this->se->streamMap) {
    auto S = IdStream.second;
    if (!S->configured) {
      continue;
    }
    currentAliveStreams++;
    assignedEntries += S->maxSize;
  }
  // * UnAssignedEntries.
  int unAssignedEntries = this->se->totalRunAheadLength - assignedEntries;
  // * BasicEntries.
  auto streamRegion = S->streamRegion;
  int totalAliveStreams = this->se->enableCoalesce
                              ? streamRegion->total_alive_coalesced_streams()
                              : streamRegion->total_alive_streams();
  int basicEntries = std::max(totalAliveStreams, currentAliveStreams) *
                     this->se->defaultRunAheadLength;
  // * AssignedBasicEntries.
  int assignedBasicEntries =
      currentAliveStreams * this->se->defaultRunAheadLength;
  // * AvailableEntries.
  int availableEntries =
      unAssignedEntries - (basicEntries - assignedBasicEntries);
  // * UpperBoundEntries.
  int upperBoundEntries =
      (this->se->totalRunAheadLength - basicEntries) / streamList.size() +
      this->se->defaultRunAheadLength;
  const auto incrementStep = 2;
  int totalIncrementEntries = incrementStep * streamList.size();

  if (availableEntries < totalIncrementEntries) {
    return;
  }
  if (totalAliveStreams * this->se->defaultRunAheadLength +
          streamList.size() * (stepRootStream->maxSize + incrementStep -
                               this->se->defaultRunAheadLength) >=
      this->se->totalRunAheadLength) {
    return;
  }
  if (stepRootStream->maxSize + incrementStep > upperBoundEntries) {
    return;
  }

  S_DPRINTF_(
      StreamThrottle, S,
      "[Throttle] AssignedEntries %d UnAssignedEntries %d BasicEntries %d "
      "AssignedBasicEntries %d AvailableEntries %d UpperBoundEntries %d "
      "TotalIncrementEntries %d CurrentAliveStreams %d TotalAliveStreams %d.\n",
      assignedEntries, unAssignedEntries, basicEntries, assignedBasicEntries,
      availableEntries, upperBoundEntries, totalIncrementEntries,
      currentAliveStreams, totalAliveStreams);

  auto oldMaxSize = S->maxSize;
  for (auto stepS : streamList) {
    // Increase the run ahead length by 2.
    stepS->maxSize += incrementStep;
  }
  assert(S->maxSize == oldMaxSize + incrementStep &&
         "RunAheadLength is not increased.");
}
