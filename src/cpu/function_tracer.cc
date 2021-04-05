#include "function_tracer.hh"

#include "base/callback.hh"
#include "base/loader/symtab.hh"
#include "base/output.hh"
#include "base/statistics.hh"
#include "sim/core.hh"

void FunctionTracer::enableFunctionTrace() {
  assert(!this->functionTracingEnabled);
  const std::string fname = csprintf("ftrace.%s", this->name);
  this->functionTraceStream = simout.findOrCreate(fname)->stream();
  this->functionTracingEnabled = true;
  this->functionEntryTick = curTick();
}

void FunctionTracer::enableFunctionAccumulateTick() {
  assert(!this->functionAccumulateTickEnabled);
  this->functionAccumulateTickEnabled = true;
  this->functionEntryTick = curTick();

  // Register stats callback.
  Stats::registerResetCallback(
      new MakeCallback<FunctionTracer,
                       &FunctionTracer::resetFuncAccumulateTick>(
          this, true /* auto delete */));
  Stats::registerDumpCallback(
      new MakeCallback<FunctionTracer, &FunctionTracer::dumpFuncAccumulateTick>(
          this, true /* auto delete */));
}

void FunctionTracer::traceFunctions(Addr pc) {

  if (!this->functionTracingEnabled && !this->functionAccumulateTickEnabled)
    return;

  if (!Loader::debugSymbolTable)
    return;

  // if pc enters different function, print new function symbol and
  // update saved range.  Otherwise do nothing.
  if (pc < this->currentFunctionStart || pc >= this->currentFunctionEnd) {
    std::string sym_str;
    auto oldFunctionStart = this->currentFunctionStart;
    bool found = Loader::debugSymbolTable->findNearestSymbol(
        pc, sym_str, this->currentFunctionStart, this->currentFunctionEnd);

    if (!found) {
      // no symbol found: use addr as label
      sym_str = csprintf("0x%x", pc);
      this->currentFunctionStart = pc;
      this->currentFunctionEnd = pc + 1;
    }

    auto accumulateTick = curTick() - this->functionEntryTick;

    if (this->functionTracingEnabled) {
      ccprintf(*this->functionTraceStream, " (%d)\n%d: %s", accumulateTick,
               curTick(), sym_str);
    }

    if (this->functionAccumulateTickEnabled) {
      this->accumulateTick(oldFunctionStart, accumulateTick);
    }

    this->functionEntryTick = curTick();
  }
  if (this->functionAccumulateTickEnabled) {
    this->accumulateMicroOps(this->currentFunctionStart, 1);
  }
}

void FunctionTracer::accumulateTick(Addr funcStart, Tick ticks) {
  this->addrFuncProfileMap
      .emplace(std::piecewise_construct, std::forward_as_tuple(funcStart),
               std::forward_as_tuple())
      .first->second.ticks += ticks;
}

void FunctionTracer::accumulateMicroOps(Addr funcStart, uint64_t microOps) {
  this->addrFuncProfileMap
      .emplace(std::piecewise_construct, std::forward_as_tuple(funcStart),
               std::forward_as_tuple())
      .first->second.microOps += microOps;
}

void FunctionTracer::resetFuncAccumulateTick() {
  this->addrFuncProfileMap.clear();
  // We also reset the function entry tick.
  this->functionEntryTick = curTick();
}

void FunctionTracer::dumpFuncAccumulateTick() {

  if (!Loader::debugSymbolTable) {
    return;
  }

  /**
   * Make sure we record the current accumulated ticks.
   */
  if (this->functionAccumulateTickEnabled && this->currentFunctionStart) {
    auto accumulateTick = curTick() - this->functionEntryTick;
    this->accumulateTick(this->currentFunctionStart, accumulateTick);
    this->functionEntryTick = curTick();
  }

  if (this->addrFuncProfileMap.empty()) {
    return;
  }

  if (!this->functionAccumulateTickStream) {
    const std::string fname = csprintf("ftick.%s", this->name);
    this->functionAccumulateTickStream = simout.findOrCreate(fname)->stream();
  }

  // Sort by ticks.
  std::vector<std::pair<Addr, FuncProfile>> sorted(
      this->addrFuncProfileMap.begin(), this->addrFuncProfileMap.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const std::pair<Addr, FuncProfile> &a,
               const std::pair<Addr, FuncProfile> &b) -> bool {
              if (a.second.ticks != b.second.ticks) {
                return a.second.ticks > b.second.ticks;
              } else {
                // Break the tie with pc.
                return a.first < b.first;
              }
            });

  // Sum all ticks.
  Tick sumTicks = 0;
  for (const auto &pcTick : sorted) {
    sumTicks += pcTick.second.ticks;
  }

  ccprintf(*this->functionAccumulateTickStream, "======================\n");
  for (const auto &pcTick : sorted) {
    auto pc = pcTick.first;
    auto tick = pcTick.second.ticks;
    auto microOps = pcTick.second.microOps;
    std::string symbol;
    if (!Loader::debugSymbolTable->findSymbol(pc, symbol)) {
      symbol = csprintf("0x%x", pc);
    }
    float percentage =
        static_cast<float>(tick) / static_cast<float>(sumTicks) * 100.f;
    ccprintf(*this->functionAccumulateTickStream, "%2.2f %20llu %15llu : %s\n",
             percentage, tick, microOps, symbol);
  }
}
