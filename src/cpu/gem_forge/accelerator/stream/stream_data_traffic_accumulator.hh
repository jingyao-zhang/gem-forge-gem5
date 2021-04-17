#ifndef __CPU_GEM_FORGE_STREAM_DATA_TRAFFIC_ACCUMULATOR_HH__
#define __CPU_GEM_FORGE_STREAM_DATA_TRAFFIC_ACCUMULATOR_HH__

#include "stream_engine.hh"

/**
 * This is used to estimate the stream data traffic.
 * It models 8x8 mesh, 8-bit link, 1kB LLC interleaving.
 * We choose 8-bit link to include the sub-line transfer benefits from
 * StreamFloat.
 */

class StreamDataTrafficAccumulator {
public:
  StreamDataTrafficAccumulator(StreamEngine *_se, bool _floated);
  void setName(const std::string &name) { myName = name; }
  void regStats();

  void commit(const std::list<Stream *> &commitStreams);

private:
  std::string myName;
  StreamEngine *se;
  const bool floated;

  const int interleaveSize = 1024;
  const int rowSize = 8;
  const int colSize = 8;
  const int flitSizeBytes = 1;

  Stats::Scalar hops;
  Stats::Scalar cachedHops;

  std::string name() const { return this->myName; }

  int getMyBank() const { return this->se->getCPUDelegator()->cpuId(); }
  int getRow(int bank) const { return bank / this->colSize; }
  int getCol(int bank) const { return bank % this->colSize; }
  int getBank(int row, int col) const { return row * this->colSize + col; }
  int mapPAddrToBank(Addr paddr) const {
    return (paddr / this->interleaveSize) % (this->rowSize * this->colSize);
  }
  int getDistance(int bankA, int bankB) const {
    int rowA = getRow(bankA);
    int rowB = getRow(bankB);
    int colA = getCol(bankA);
    int colB = getCol(bankB);
    return std::abs(rowA - rowB) + std::abs(colA - colB);
  }
  int getNumFlits(int bytes) const {
    return (bytes + this->flitSizeBytes - 1) / this->flitSizeBytes;
  }

  /**
   * Get element data bank.
   * @return -1 if faulted on the element's vaddr.
   */
  int getElementDataBank(const StreamElement *element);
  void computeTrafficFix(const StreamElement *element);
  void computeTrafficFloat(const StreamElement *element);
};

#endif