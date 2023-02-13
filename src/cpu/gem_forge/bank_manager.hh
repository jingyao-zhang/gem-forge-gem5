#ifndef __CPU_TDG_BANK_MANAGER_HH__
#define __CPU_TDG_BANK_MANAGER_HH__

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace gem5 {

/**
 * A simple class to model the bank conflictions in one cycle.
 *
 * Each cache line can be divided in to N bank, each bank can have M ports.
 */

class BankManager {
 public:
  BankManager(uint32_t _cacheLineSize, uint32_t _numBanks,
              uint32_t _numPortsPerBank);

  /**
   * Clear usedPorts for a new cycle.
   */
  void clear();

  bool isNonConflict(uint64_t addr, uint64_t size) const;

  /**
   * Add this access to the usedPorts.
   */
  void access(uint64_t addr, uint64_t size);

 private:
  uint32_t cacheLineSize;
  uint32_t numBanks;
  uint32_t numPortsPerBank;

  uint32_t bytesPerBank;

  std::vector<int> usedPorts;

  std::pair<uint32_t, uint32_t> getBanks(uint64_t addr, uint64_t size) const;
};

} // namespace gem5

#endif
