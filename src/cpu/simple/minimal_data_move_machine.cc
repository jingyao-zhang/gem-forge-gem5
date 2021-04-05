#include "minimal_data_move_machine.hh"

#include "base/callback.hh"
#include "base/loader/symtab.hh"
#include "base/output.hh"

// For rand().
#include <cstdlib>

#include "debug/MinimalDataMoveMachine.hh"

std::vector<std::string> MinimalDataMoveMachine::ignoredFuncs{
    // "__kmp_hyper_barrier_release",
    // "__kmp_hardware_timestamp",
    // "__kmp_join_barrier",
    // "__kmp_barrier_template",
    // "__kmp_fork_call",
    "__kmp_",
    "__kmpc_",
};

MinimalDataMoveMachine::MinimalDataMoveMachine(const std::string &_name,
                                               int _bank, bool _fixedBank)
    : myName(_name), bank(_bank), fixedBank(_fixedBank) {}

void MinimalDataMoveMachine::regStats() {
  this->totalHops.name(this->myName + ".totalHops")
      .desc("TotalHops of the MinimalDataMoveMachine")
      .flags(Stats::nozero);
  this->totalIgnoredHops.name(this->myName + ".totalIgnoredHops")
      .desc("TotalIngoredHops of the MinimalDataMoveMachine")
      .flags(Stats::nozero);
  this->totalStreamHops.name(this->myName + ".totalStreamHops")
      .desc("TotalStreamHops of the MinimalDataMoveMachine")
      .flags(Stats::nozero);

  Stats::registerResetCallback(
      new MakeCallback<MinimalDataMoveMachine,
                       &MinimalDataMoveMachine::resetPCHopsMap>(
          this, true /* auto delete */));
  Stats::registerDumpCallback(
      new MakeCallback<MinimalDataMoveMachine,
                       &MinimalDataMoveMachine::dumpPCHopsMap>(
          this, true /* auto delete */));
}

void MinimalDataMoveMachine::commit(StaticInstPtr staticInst,
                                    const TheISA::PCState &pc, Addr paddr,
                                    bool isStream) {

  // Collect source reg information.
  std::map<int, std::set<RegId>> bankToSrcRegMap;
  bool allSrcRegsAreStream = true;
  for (int i = 0; i < staticInst->numSrcRegs(); ++i) {
    const auto &srcRegId = staticInst->srcRegIdx(i);
    auto &dynRegInfo = this->getDynRegInfo(srcRegId);
    /**
     * We just ignore MiscReg and CCReg.
     */
    if (srcRegId.isCCReg() || srcRegId.isMiscReg()) {
      continue;
    }
    bankToSrcRegMap
        .emplace(std::piecewise_construct,
                 std::forward_as_tuple(dynRegInfo.bank),
                 std::forward_as_tuple())
        .first->second.insert(srcRegId);
    if (!dynRegInfo.isStream) {
      allSrcRegsAreStream = false;
    }
  }

  int finalBank = this->bank;
  if (!this->fixedBank && !bankToSrcRegMap.empty()) {
    finalBank = this->getMiddlePoint(bankToSrcRegMap);
  }
  if (paddr != 0) {
    finalBank = this->mapPAddrToBank(paddr);
  }
  auto traffic = this->getTraffic(bankToSrcRegMap, finalBank);
  bool ignored = this->shouldIgnoreTraffic(pc.pc());
  if (!ignored) {
    this->totalHops += traffic.first;
    this->totalStreamHops += traffic.second;
    this->pcHopsMap.emplace(pc.pc(), 0).first->second += traffic.first;
  } else {
    this->totalIgnoredHops += traffic.first;
  }

  DPRINTF(MinimalDataMoveMachine,
          "%s Ignored %d IsStream %d FinalBank %d Hops %d StreamHops %d.\n",
          staticInst->disassemble(pc.pc()), ignored, isStream, finalBank,
          traffic.first, traffic.second);

  for (int i = 0; i < staticInst->numDestRegs(); ++i) {
    const auto &destRegId = staticInst->destRegIdx(i);
    auto &dynRegInfo = this->getDynRegInfo(destRegId);
    dynRegInfo.bank = finalBank;
    dynRegInfo.isStream = (isStream || allSrcRegsAreStream);
  }
}

int MinimalDataMoveMachine::getMiddlePoint(const BankRegMap &bankToRegMap) {
  int weightedRow = 0;
  int weightedCol = 0;
  int weight = 0;
  for (const auto &entry : bankToRegMap) {
    auto bank = entry.first;
    auto row = this->getRow(bank);
    auto col = this->getCol(bank);
    weightedRow += row * entry.second.size();
    weightedCol += col * entry.second.size();
    weight += entry.second.size();
  }
  int middleRow = this->randomDivide(weightedRow, weight);
  int middleCol = this->randomDivide(weightedCol, weight);
  int middleBank = this->getBank(middleRow, middleCol);
  return middleBank;
}

std::pair<int, int>
MinimalDataMoveMachine::getTraffic(const BankRegMap &bankToRegMap,
                                   int destBank) {
  int totalHops = 0;
  int totalStreamHops = 0;
  auto destRow = this->getRow(destBank);
  auto destCol = this->getCol(destBank);
  for (const auto &entry : bankToRegMap) {
    auto bank = entry.first;
    auto row = this->getRow(bank);
    auto col = this->getCol(bank);
    auto hops = std::abs(row - destRow) + std::abs(col - destCol);
    auto dataBytes = entry.second.size() * sizeof(uint64_t);
    auto flits = this->getNumFlits(dataBytes);
    totalHops += hops * flits;
    auto streamDataBytes = 0;
    for (const auto &regId : entry.second) {
      const auto &dynRegInfo = this->getDynRegInfo(regId);
      DPRINTF(MinimalDataMoveMachine, "   src %s bank %d.\n", regId, bank);
      if (dynRegInfo.isStream) {
        streamDataBytes += sizeof(uint64_t);
      }
    }
    auto streamFlits = this->getNumFlits(streamDataBytes);
    totalStreamHops += hops * streamFlits;
  }
  return std::make_pair(totalHops, totalStreamHops);
}

MinimalDataMoveMachine::DynRegInfo &
MinimalDataMoveMachine::getDynRegInfo(const RegId &regId) {
  return this->regInfoMap
      .emplace(std::piecewise_construct, std::forward_as_tuple(regId),
               std::forward_as_tuple(this->bank, false))
      .first->second;
}

int MinimalDataMoveMachine::randomDivide(int A, int B) const {
  auto f = static_cast<float>(A) / static_cast<float>(B);
  auto i = A / B;
  auto fraction = f - i;
  if (fraction > 0.4f && fraction < 0.6f) {
    if (rand() % 2) {
      i = i + 1;
    }
  } else if (fraction >= 0.6f) {
    i = i + 1;
  }
  return i;
}

bool MinimalDataMoveMachine::shouldIgnoreTraffic(Addr pc) {
  auto iter = this->pcIgnoredMap.find(pc);
  if (iter == this->pcIgnoredMap.end()) {
    bool ignore = false;
    Addr funcStart = 0;
    Addr funcEnd = 0;
    std::string symbol;
    bool found = Loader::debugSymbolTable->findNearestSymbol(
        pc, symbol, funcStart, funcEnd);
    if (!found) {
      symbol = csprintf("0x%x", pc);
    }
    for (const auto &f : ignoredFuncs) {
      auto pos = symbol.find(f);
      if (pos != std::string::npos) {
        ignore = true;
        break;
      }
    }
    iter = this->pcIgnoredMap.emplace(pc, ignore).first;
  }
  return iter->second;
}

void MinimalDataMoveMachine::resetPCHopsMap() { this->pcHopsMap.clear(); }

void MinimalDataMoveMachine::dumpPCHopsMap() {
  if (!Loader::debugSymbolTable) {
    return;
  }

  /**
   * Make sure we record the current accumulated ticks.
   */
  if (this->pcHopsMap.empty()) {
    return;
  }

  if (!this->pcHopsStream) {
    auto dir = simout.findOrCreateSubdirectory("min_data");
    const std::string fname = csprintf("%s.txt", this->myName);
    this->pcHopsStream = dir->findOrCreate(fname)->stream();
  }

  // Sort by ticks.
  std::vector<std::pair<Addr, uint64_t>> sorted(this->pcHopsMap.begin(),
                                                this->pcHopsMap.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const std::pair<Addr, uint64_t> &a,
               const std::pair<Addr, uint64_t> &b) -> bool {
              if (a.second != b.second) {
                return a.second > b.second;
              } else {
                // Break the tie with pc.
                return a.first < b.first;
              }
            });

  Tick sumHops = 0;
  for (const auto &pcHops : sorted) {
    sumHops += pcHops.second;
  }

  ccprintf(*this->pcHopsStream, "======================\n");
  for (const auto &pcTick : sorted) {
    auto pc = pcTick.first;
    auto tick = pcTick.second;
    Addr funcStart = 0;
    Addr funcEnd = 0;
    std::string symbol;
    bool found = Loader::debugSymbolTable->findNearestSymbol(
        pc, symbol, funcStart, funcEnd);
    if (!found) {
      symbol = csprintf("0x%x", pc);
    }
    float percentage =
        static_cast<float>(tick) / static_cast<float>(sumHops) * 100.f;
    ccprintf(*this->pcHopsStream, "%#x %2.2f %20llu : %s\n", pc, percentage,
             tick, symbol);
  }
}