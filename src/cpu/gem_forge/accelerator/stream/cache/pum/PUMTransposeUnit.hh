#ifndef __CPU_GEM_FORGE_PUM_TRANSPOSE_UNIT_HH__
#define __CPU_GEM_FORGE_PUM_TRANSPOSE_UNIT_HH__

#include "PUMHWConfiguration.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include <vector>

/**
 * This class model the overheads of reading from a PUM transposed array.
 */

class PUMTransposeUnit {
public:
  PUMTransposeUnit(AbstractStreamAwareController *_controller)
      : controller(_controller) {}

  /**
   * @brief Record this cache line and return the adjusted latency.
   * Currently, this is used to charge extra latency to access cache lines
   * in transposed layout in in-memory computing.
   *
   * @return Cycles.
   */
  Cycles access(Addr paddrLine, Cycles defaultLat);

private:
  AbstractStreamAwareController *controller;

  bool initialized = false;

  /**
   * Model the contention to each way.
   */
  std::vector<Cycles> readyCycles;

  void initialize();
};

#endif