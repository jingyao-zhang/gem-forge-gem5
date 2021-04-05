#ifndef __CPU_SIMPLE_MINIMAL_DATA_MOVE_MACHINE_HH__
#define __CPU_SIMPLE_MINIMAL_DATA_MOVE_MACHINE_HH__

/**
 * This is an ideal machine that can move the computation
 * at instruction granularity to minimize the data movement.
 * You can think of it as a gigantic dataflow machine.
 *
 * TODO: Only 8x8 mesh topology is supported for now.
 */

#include "base/statistics.hh"
#include "config/the_isa.hh"
#include "cpu/reg_class.hh"
#include "cpu/static_inst.hh"

#include <map>
#include <set>

class MinimalDataMoveMachine {
public:
  MinimalDataMoveMachine(const std::string &_name, int _bank, bool _fixedBank);

  void regStats();

  /**
   * Record one instruction and update my stats.
   */
  void commit(StaticInstPtr staticInst, Addr paddr, bool isStream);

private:
  /**
   * My bank id.
   */
  const std::string name;
  const int bank;
  const bool fixedBank;

  const int interleaveSize = 64;
  const int rowSize = 8;
  const int colSize = 8;
  const int flitSizeBytes = 32;

  Stats::Scalar totalHops;
  Stats::Scalar totalStreamHops;

  /**
   * This represents the dynamic information of the register.
   * 1. Which bank it is now.
   * 2. Whether it is originated from a stream.
   */
  struct DynRegInfo {
    int bank;
    bool isStream;
    DynRegInfo(int _bank, bool _isStream) : bank(_bank), isStream(_isStream) {}
  };

  std::map<RegId, DynRegInfo> regInfoMap;

  /**
   * Look up the DynRegInfo from RegId. If not found, initialize to this bank
   * and isStream = false.
   */
  DynRegInfo &getDynRegInfo(const RegId &regId);

  using BankRegMap = std::map<int, std::set<RegId>>;
  int getMiddlePoint(const BankRegMap &bankToRegMap);
  std::pair<int, int> getTraffic(const BankRegMap &bankToRegMap, int destBank);

  int getRow(int bank) const { return bank / this->colSize; }
  int getCol(int bank) const { return bank % this->colSize; }
  int getBank(int row, int col) const { return row * this->colSize + col; }
  int mapPAddrToBank(Addr paddr) const {
    return (paddr / this->interleaveSize) % (this->rowSize * this->colSize);
  }
  int getNumFlits(int bytes) const {
    return (bytes + this->flitSizeBytes - 1) / this->flitSizeBytes;
  }
};

#endif
