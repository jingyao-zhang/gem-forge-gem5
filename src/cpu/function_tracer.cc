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
      this->funcAccumulateTicks.emplace(oldFunctionStart, 0).first->second +=
          accumulateTick;
    }

    this->functionEntryTick = curTick();
  }
}

void FunctionTracer::resetFuncAccumulateTick() {
  this->funcAccumulateTicks.clear();
  // We also reset the function entry tick.
  this->functionEntryTick = curTick();
}

void FunctionTracer::dumpFuncAccumulateTick() {

  if (!Loader::debugSymbolTable)
    return;

  if (!this->functionAccumulateTickStream) {
    const std::string fname = csprintf("ftick.%s", this->name);
    this->functionAccumulateTickStream = simout.findOrCreate(fname)->stream();
  }

  // Sort by ticks.
  std::vector<std::pair<Addr, Tick>> sorted(this->funcAccumulateTicks.begin(),
                                            this->funcAccumulateTicks.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const std::pair<Addr, Tick> &a,
               const std::pair<Addr, Tick> &b) -> bool {
              if (a.second != b.second) {
                return a.second > b.second;
              } else {
                // Break the tie with pc.
                return a.first < b.first;
              }
            });

  // Sum all ticks.
  Tick sumTicks = 0;
  for (const auto &pcTick : sorted) {
    sumTicks += pcTick.second;
  }

  ccprintf(*this->functionAccumulateTickStream, "======================\n");
  for (const auto &pcTick : sorted) {
    auto pc = pcTick.first;
    auto tick = pcTick.second;
    std::string symbol;
    if (!Loader::debugSymbolTable->findSymbol(pc, symbol)) {
      symbol = csprintf("0x%x", pc);
    }
    float percentage =
        static_cast<float>(tick) / static_cast<float>(sumTicks) * 100.f;
    ccprintf(*this->functionAccumulateTickStream, "%2.2f %20llu: %s\n",
             percentage, tick, symbol);
  }
}
