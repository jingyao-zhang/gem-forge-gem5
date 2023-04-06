#include "stream_inner_loop_dep.hh"
#include "stream.hh"

#include "base/logging.hh"

#include "debug/StreamBase.hh"
#define DEBUG_TYPE StreamBase
#include "stream_log.hh"

namespace gem5 {

void StreamInnerLoopDepTracker::trackAsInnerLoopBase(StaticId baseStaticId,
                                                     StreamDepType type) {
  this->innerLoopBaseEdges.emplace(std::piecewise_construct,
                                   std::forward_as_tuple(baseStaticId),
                                   std::forward_as_tuple());
  this->innerLoopBaseType.emplace(baseStaticId, type);
}

bool StreamInnerLoopDepTracker::isTrackedAsInnerLoopBase(
    StaticId baseStaticId) const {
  return this->innerLoopBaseEdges.count(baseStaticId);
}

DynStreamEdges &
StreamInnerLoopDepTracker::getInnerLoopBaseEdges(StaticId baseStaticId) {
  auto iter = this->innerLoopBaseEdges.find(baseStaticId);
  if (iter == this->innerLoopBaseEdges.end()) {
    DYN_S_PANIC(this->dynId,
                "Failed to find InnerLoopBaseEdges for StaticId %lu.",
                baseStaticId);
  }
  return iter->second;
}

const DynStreamEdges &
StreamInnerLoopDepTracker::getInnerLoopBaseEdges(StaticId baseStaticId) const {
  auto iter = this->innerLoopBaseEdges.find(baseStaticId);
  if (iter == this->innerLoopBaseEdges.end()) {
    DYN_S_PANIC(this->dynId,
                "Failed to find InnerLoopBaseEdges for StaticId %lu.",
                baseStaticId);
  }
  return iter->second;
}

bool StreamInnerLoopDepTracker::checkInnerLoopBaseElems(
    const FIFOEntryIdx &FIFOIdx, bool checkValueReady) {

  auto elemIdx = FIFOIdx.entryIdx;

  S_FIFO_ENTRY_DPRINTF(FIFOIdx,
                       "[InnerLoopDep] Checking InnerLoopBaseElems.\n");
  for (const auto &entry : this->innerLoopBaseEdges) {

    auto baseId = entry.first;
    auto baseType = this->innerLoopBaseType.at(baseId);
    if (baseType == DynStreamDepEdge::Back && elemIdx == 0) {
      continue;
    }

    const auto &dynEdges = entry.second;

    auto adjustElemIdx =
        baseType == DynStreamDepEdge::Back ? (elemIdx - 1) : elemIdx;
    if (adjustElemIdx >= dynEdges.size()) {
      // The inner-loop stream has not been allocated.
      S_FIFO_ENTRY_DPRINTF(
          FIFOIdx,
          "[InnerLoopDep]   InnerLoopS %d AdjElemIdx %lu Not Config.\n", baseId,
          adjustElemIdx);
      return false;
    }

    const auto &dynEdge = dynEdges.at(adjustElemIdx);
    auto baseS = dynEdge.baseS;

    auto &baseDynS = baseS->getDynStreamByInstance(dynEdge.baseInstanceId);
    S_FIFO_ENTRY_DPRINTF(FIFOIdx, "[InnerLoopDep]   InnerLoopDynS %s.\n",
                         baseDynS.dynStreamId);
    if (!baseDynS.hasTotalTripCount()) {
      S_FIFO_ENTRY_DPRINTF(FIFOIdx, "[InnerLoopDep]   NoTripCount.\n");
      return false;
    }
    auto baseTripCount = baseDynS.getTotalTripCount();
    if (baseDynS.FIFOIdx.entryIdx <= baseTripCount) {
      S_FIFO_ENTRY_DPRINTF(
          FIFOIdx, "[InnerLoopDep]   TripCount %llu > BaseDynS NextElem %lu.\n",
          baseTripCount, baseDynS.FIFOIdx.entryIdx);
      return false;
    }
    auto baseElement = baseDynS.getElemByIdx(baseTripCount);
    if (!baseElement) {
      S_FIFO_ENTRY_PANIC(
          FIFOIdx,
          "[InnerLoopDep]   InnerLoopDynS %s TripCount %llu ElemReleased.",
          baseDynS.dynStreamId, baseTripCount);
    }
    if (checkValueReady && !baseElement->isValueReady) {
      S_FIFO_ENTRY_DPRINTF(FIFOIdx,
                           "[InnerLoopDep]   BaseElem %s NotValueReady.\n",
                           baseElement->FIFOIdx);
      return false;
    }
  }

  return true;
}

void StreamInnerLoopDepTracker::getInnerLoopBaseElems(
    const FIFOEntryIdx &FIFOIdx, BaseStreamElemVec &baseElems) {

  auto elemIdx = FIFOIdx.entryIdx;
  for (const auto &entry : this->innerLoopBaseEdges) {

    auto baseId = entry.first;
    auto baseType = this->innerLoopBaseType.at(baseId);

    if (baseType == DynStreamDepEdge::Back && elemIdx == 0) {
      continue;
    }
    const auto &dynEdges = entry.second;

    auto adjustElemIdx =
        baseType == DynStreamDepEdge::Back ? (elemIdx - 1) : elemIdx;
    if (adjustElemIdx >= dynEdges.size()) {
      // The inner-loop stream has not been allocated.
      S_FIFO_ENTRY_PANIC(
          FIFOIdx, "[InnerLoopDep] InnerLoopS %d AdjElemIdx %lu Not Config.\n",
          baseId, adjustElemIdx);
    }

    const auto &dynEdge = dynEdges.at(adjustElemIdx);
    auto baseS = dynEdge.baseS;
    auto &baseDynS = baseS->getDynStreamByInstance(dynEdge.baseInstanceId);
    if (!baseDynS.hasTotalTripCount()) {
      S_FIFO_ENTRY_PANIC(FIFOIdx, "[InnerLoopDep] NoTripCount.\n");
    }
    auto baseTripCount = baseDynS.getTotalTripCount();
    if (baseDynS.FIFOIdx.entryIdx <= baseTripCount) {
      S_FIFO_ENTRY_PANIC(
          FIFOIdx, "[InnerLoopDep] TripCount %llu > BaseDynS NextElem %lu.\n",
          baseTripCount, baseDynS.FIFOIdx.entryIdx);
    }
    assert(baseTripCount > 0 && "0 TripCount.");
    auto baseElemIdx = baseTripCount;
    if (baseS->isInnerSecondFinalValueUsedByCore()) {
      baseElemIdx = baseTripCount - 1;
    }
    auto baseElem = baseDynS.getElemByIdx(baseElemIdx);
    if (!baseElem) {
      S_FIFO_ENTRY_PANIC(
          FIFOIdx,
          "[InnerLoopDep] %s TripCount %llu BaseElemIdx %llu Released.",
          baseDynS.dynStreamId, baseTripCount, baseElemIdx);
    }
    S_FIFO_ENTRY_DPRINTF(FIFOIdx, "[InnerLoopDep] Add Type %s BaseElem: %s.\n",
                         baseType, baseElem->FIFOIdx);
    baseDynS.popInnerLoopDepS(this->dynId);
    baseElems.emplace_back(baseType, baseElem);
  }
}

void StreamInnerLoopDepTracker::releaseAllInnerLoopBase() {
  for (auto &entry : this->innerLoopBaseEdges) {
    auto &dynEdges = entry.second;
    for (const auto &dynEdge : dynEdges) {
      auto baseS = dynEdge.baseS;
      if (auto baseDynS =
              baseS->tryGetDynStreamByInstance(dynEdge.baseInstanceId)) {
        baseDynS->tryPopInnerLoopDepS(this->dynId);
        DYN_S_DPRINTF(this->dynId, "[InnerLoopDep] Drop %s.\n",
                      baseDynS->dynStreamId);
      }
    }
    dynEdges.clear();
  }
}

void StreamInnerLoopDepTracker::pushInnerLoopBaseDynStream(
    DynStreamDepEdge::TypeE type, Stream *baseS, StaticId baseStaticId,
    InstanceId baseInstanceId, StaticId depStaticId) {
  auto &edges = this->getInnerLoopBaseEdges(baseStaticId);
  edges.emplace_back(type, baseS, baseStaticId, baseInstanceId, depStaticId,
                     0 /* AlignBaseElementIdx */, 1 /* ReuseBaseElement */);
}

void StreamInnerLoopDepTracker::popInnerLoopDepS(const DynStreamId &dynId) {
  assert(this->tryPopInnerLoopDepS(dynId) && "No InnerLoopDepS");
}

bool StreamInnerLoopDepTracker::tryPopInnerLoopDepS(const DynStreamId &dynId) {
  for (auto iter = this->innerLoopDepDynIds.begin();
       iter != this->innerLoopDepDynIds.end(); ++iter) {
    if (iter->dynId == dynId) {
      this->innerLoopDepDynIds.erase(iter);
      return true;
    }
  }
  return false;
}

} // namespace gem5