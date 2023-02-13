#include "bank_manager.hh"

namespace gem5 {

BankManager::BankManager(uint32_t _cacheLineSize, uint32_t _numBanks,
                         uint32_t _numPortsPerBank)
    : cacheLineSize(_cacheLineSize),
      numBanks(_numBanks),
      numPortsPerBank(_numPortsPerBank) {
  assert((this->cacheLineSize > 0) && "Illegal cache line size.");
  assert((this->numBanks > 0) && "Illegal number of banks.");
  assert((this->numPortsPerBank > 0) && "Illegal number of ports per bank.");
  assert((this->cacheLineSize % this->numBanks == 0) &&
         "Illegal cache line size and number of banks.");

  this->bytesPerBank = this->cacheLineSize / this->numBanks;

  this->usedPorts.resize(this->numBanks, 0);
}

void BankManager::clear() {
  for (auto &used : this->usedPorts) {
    used = 0;
  }
}

bool BankManager::isNonConflict(uint64_t addr, uint64_t size) const {
  auto banks = this->getBanks(addr, size);
  auto lhsBank = banks.first;
  auto rhsBank = banks.second;
  for (auto bank = lhsBank; bank <= rhsBank; ++bank) {
    if (this->usedPorts[bank] == this->numPortsPerBank) {
      return false;
    }
  }
  return true;
}

void BankManager::access(uint64_t addr, uint64_t size) {
  auto banks = this->getBanks(addr, size);
  auto lhsBank = banks.first;
  auto rhsBank = banks.second;
  for (auto bank = lhsBank; bank <= rhsBank; ++bank) {
    assert(this->usedPorts[bank] < this->numPortsPerBank &&
           "Bank already full.");
    this->usedPorts[bank]++;
  }
}

std::pair<uint32_t, uint32_t> BankManager::getBanks(uint64_t addr,
                                                    uint64_t size) const {
  assert(size > 0 && "Zero size access for bank manager");
  assert((addr / this->cacheLineSize) ==
             ((addr + size - 1) / this->cacheLineSize) &&
         "BankManager: access accross multiple cache lines.");
  auto lhsBank = (addr % this->cacheLineSize) / this->bytesPerBank;
  auto rhsBank = ((addr + size - 1) % this->cacheLineSize) / this->bytesPerBank;
  assert(lhsBank < this->numBanks && "BankManager: illegal lhs bank.");
  assert(rhsBank < this->numBanks && "BankManager: illegal rhs bank.");
  return std::make_pair<uint32_t, uint32_t>(lhsBank, rhsBank);
}
} // namespace gem5

