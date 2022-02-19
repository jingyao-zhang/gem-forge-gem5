#include "stream_data_traffic_accumulator.hh"

StreamDataTrafficAccumulator::StreamDataTrafficAccumulator(StreamEngine *_se,
                                                           bool _floated)
    : myName("acc"), se(_se), floated(_floated) {}

void StreamDataTrafficAccumulator::regStats() {
#define scalar(stat, describe)                                                 \
  this->stat.name(this->name() + ("." #stat)).desc(describe).prereq(this->stat)

  scalar(hops, "Accumulated data hops.");
  scalar(cachedHops, "Accumulated data hops with ideal cache.");
}

void StreamDataTrafficAccumulator::commit(
    const std::list<Stream *> &commitStreams) {

  for (auto S : commitStreams) {
    const auto &dynS = S->getFirstDynStream();
    auto element = dynS.getFirstElement();
    assert(element && "Missing FirstElement when commit.");

    if (this->floated) {
      this->computeTrafficFloat(element);
    } else {
      this->computeTrafficFix(element);
    }
  }
}

void StreamDataTrafficAccumulator::computeTrafficFix(
    const StreamElement *element) {
  /**
   * Fix is simple, just charge the traffic for all LoadStream
   * and StoreComputeStream, AtomicComputeStream.
   * Non-binding Store/AtomicStream will be charged in core, and are
   * not considered as stream traffic.
   */
  auto S = element->stream;
  //   auto hasCoreUser = S->hasCoreUser();
  //   auto coreUsed = element->isFirstUserDispatched();

  if (!(S->isLoadStream() || S->isStoreComputeStream() ||
        S->isAtomicComputeStream())) {
    // Not our target streams.
    return;
  }

  auto size = S->getMemElementSize();
  auto dataBank = this->getElementDataBank(element);
  if (dataBank == -1) {
    return;
  }

  auto myBank = this->getMyBank();
  auto distance = this->getDistance(myBank, dataBank);
  auto flits = this->getNumFlits(size);

  auto &ideaCache = this->se->getCPUDelegator()->ideaCache;
  assert(ideaCache && "Missing idea cache.");
  auto vaddr = element->addr;
  Addr paddr;
  assert(this->se->getCPUDelegator()->translateVAddrOracle(vaddr, paddr));
  auto missFlits = ideaCache->access(paddr, size);

  auto totalHops = flits * distance;
  auto totalMissHops = missFlits * distance;
  if (S->isUpdateStream() || S->isAtomicComputeStream()) {
    // These have double traffic: load and store.
    totalHops *= 2;
    totalMissHops *= 2;
  }
  this->hops += totalHops;
  this->cachedHops += totalMissHops;
  S->statistic.idealDataTrafficFix += totalHops;
  S->statistic.idealDataTrafficCached += totalHops;
}

void StreamDataTrafficAccumulator::computeTrafficFloat(
    const StreamElement *element) {

  /**
   * Float is more difficult.
   * 1. Again we only charge for LoadStream, StoreComputeStream,
   * AtomicComputeStream.
   * 2. Charge the data hops from its AddrBaseElements and ValueBaseElements to
   * its bank.
   * 3. If this is a Stream with CoreUser, charge the CoreElementSize traffic to
   * my bank.
   */

  auto S = element->stream;

  if (!(S->isLoadStream() || S->isStoreComputeStream() ||
        S->isAtomicComputeStream())) {
    // Not our target streams.
    return;
  }

  auto dataBank = this->getElementDataBank(element);
  if (dataBank == -1) {
    return;
  }
  std::unordered_set<StreamElement *> chargedBaseElements;
  int addrBaseHops = 0;
  for (const auto &baseElement : element->addrBaseElements) {
    auto baseE = baseElement.getElement();
    if (chargedBaseElements.count(baseE)) {
      continue;
    }
    auto baseS = baseE->stream;
    if (!baseS->isMemStream()) {
      continue;
    }
    auto baseDataBank = this->getElementDataBank(baseE);
    if (baseDataBank == -1) {
      continue;
    }
    auto distance = this->getDistance(dataBank, baseDataBank);
    auto flits = this->getNumFlits(baseS->getMemElementSize());
    addrBaseHops += distance * flits;
    chargedBaseElements.insert(baseE);
  }
  int valueBaseHops = 0;
  for (const auto &baseElement : element->valueBaseElements) {
    auto baseE = baseElement.getElement();
    if (chargedBaseElements.count(baseE)) {
      continue;
    }
    auto baseS = baseE->stream;
    if (!baseS->isMemStream() || baseE == element) {
      continue;
    }
    auto baseDataBank = this->getElementDataBank(baseE);
    if (baseDataBank == -1) {
      continue;
    }
    auto distance = this->getDistance(dataBank, baseDataBank);
    auto flits = this->getNumFlits(baseS->getCoreElementSize());
    valueBaseHops += distance * flits;
    chargedBaseElements.insert(baseE);
  }
  /**
   * Finally, if I have core user, I charge the traffic to get the data here.
   */
  auto hasCoreUser = S->hasCoreUser();
  auto coreUsed = element->isFirstUserDispatched();
  int toCoreHops = 0;
  if (hasCoreUser && coreUsed) {
    auto coreElementSize = S->getCoreElementSize();
    int myBank = this->getMyBank();
    auto distance = this->getDistance(dataBank, myBank);
    auto flits = this->getNumFlits(coreElementSize);
    toCoreHops += distance * flits;
  }
  auto totalHops = addrBaseHops + valueBaseHops + toCoreHops;
  this->hops += totalHops;
  S->statistic.idealDataTrafficFloat += totalHops;
}

int StreamDataTrafficAccumulator::getElementDataBank(
    const StreamElement *element) {
  auto vaddr = element->addr;
  Addr paddr;
  if (!this->se->getCPUDelegator()->translateVAddrOracle(vaddr, paddr)) {
    // This one faulted. Ignore it.
    return -1;
  }
  return this->mapPAddrToBank(paddr);
}