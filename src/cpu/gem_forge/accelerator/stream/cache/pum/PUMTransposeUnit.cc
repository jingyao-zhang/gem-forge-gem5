#include "PUMTransposeUnit.hh"

#include "sim/stream_nuca/stream_nuca_map.hh"

#include "debug/PUMTransposeUnit.hh"

Cycles PUMTransposeUnit::access(Addr paddrLine, Cycles defaultLat) {

  this->initialize();

  auto range = StreamNUCAMap::getRangeMapContaining(paddrLine);
  DPRINTF(PUMTransposeUnit, "[PUMTranspose] Addr %#x DefaultLat %d.\n",
          paddrLine, defaultLat);
  if (!range || !range->isStreamPUM) {
    return defaultLat;
  }
  // Charge the number of wordlines as the latency.
  Cycles wordlineLat(range->elementBits);

  // Reverse map from elem to bitlines.
  auto loc = StreamNUCAMap::getPUMLocation(paddrLine, *range);

  // So far we model the contention to each way.
  auto &readyCycle = this->readyCycles.at(loc.way);
  auto curCycle = this->controller->curCycle();
  Cycles delayCycles(0);

  // Update the stats.
  this->controller->m_statPUMNormalAccesses++;

  if (readyCycle > curCycle) {
    delayCycles = readyCycle - curCycle;
    this->controller->m_statPUMNormalAccessConflicts++;
    this->controller->m_statPUMNormalAccessDelayCycles += delayCycles;
  }

  Cycles lat = delayCycles + wordlineLat;
  readyCycle = curCycle + lat;

  return lat;
}

void PUMTransposeUnit::initialize() {
  if (this->initialized) {
    return;
  }

  auto hwConfig = PUMHWConfiguration::getPUMHWConfig();
  auto numWays = hwConfig.way_per_bank;
  this->readyCycles.resize(numWays, Cycles(0));
  this->initialized = true;
}