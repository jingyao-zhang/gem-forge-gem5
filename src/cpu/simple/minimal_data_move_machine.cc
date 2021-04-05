#include "minimal_data_move_machine.hh"

MinimalDataMoveMachine::MinimalDataMoveMachine(const std::string &_name,
                                               int _bank, bool _fixedBank)
    : name(_name), bank(_bank), fixedBank(_fixedBank) {}

void MinimalDataMoveMachine::regStats() {
  this->totalHops.name(this->name + ".totalHops")
      .desc("TotalHops of the MinimalDataMoveMachine")
      .flags(Stats::nozero);
  this->totalStreamHops.name(this->name + ".totalStreamHops")
      .desc("TotalStreamHops of the MinimalDataMoveMachine")
      .flags(Stats::nozero);
}

void MinimalDataMoveMachine::commit(StaticInstPtr staticInst, Addr paddr,
                                    bool isStream) {

  // Collect source reg information.
  std::map<int, std::set<RegId>> bankToSrcRegMap;
  bool allSrcRegsAreStream = true;
  for (int i = 0; i < staticInst->numSrcRegs(); ++i) {
    const auto &srcRegId = staticInst->srcRegIdx(i);
    auto &dynRegInfo = this->getDynRegInfo(srcRegId);
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
    // hack("%s FinalBank %d\n", staticInst->disassemble(0), finalBank);
  }
  if (paddr != 0) {
    finalBank = this->mapPAddrToBank(paddr);
  }
  auto traffic = this->getTraffic(bankToSrcRegMap, finalBank);
  this->totalHops += traffic.first;
  this->totalStreamHops += traffic.second;

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
  int middleRow = weightedRow / weight;
  int middleCol = weightedCol / weight;
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