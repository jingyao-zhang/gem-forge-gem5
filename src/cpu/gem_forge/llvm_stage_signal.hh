#ifndef __CPU_LLVM_STAGE_SIGNAL__
#define __CPU_LLVM_STAGE_SIGNAL__

#include "common.hh"

#include <array>
#include <cassert>

namespace gem5 {

struct LLVMStageSignal {
  // Per-context stall signal;
  std::array<bool, LLVMTraceCPUConstants::MaxContexts> contextStall;
  LLVMStageSignal() {
    for (auto &stall : this->contextStall) {
      stall = false;
    }
  }
};

} // namespace gem5

#endif