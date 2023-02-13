#include "cpu/gem_forge/llvm_branch_predictor.hh"
#include "base/trace.hh"
#include "debug/LLVMBranchPredictor.hh"

namespace gem5 {

bool LLVMBranchPredictor::predictAndUpdate(const LLVMDynamicInst *inst) {
  if (!inst->isBranchInst()) {
    panic("This is not a branch inst %s.\n", inst->getInstName().c_str());
  }
  bool result = this->predict(inst);
  this->update(inst);

  DPRINTF(LLVMBranchPredictor, "Predict to inst %p, %s, result %d.\n",
          reinterpret_cast<void *>(inst->getPC()), inst->getInstName().c_str(),
          result);
  return result;
}

bool LLVMBranchPredictor::predict(const LLVMDynamicInst *inst) const {
  auto pc = inst->getPC();
  auto dynamicNextPC = inst->getDynamicNextPC();
  auto iter = this->records.find(pc);
  if (iter == this->records.end()) {
    // This is a new inst, always predict wrong?
    return false;
  } else {
    // Check if we predict it right.
    return dynamicNextPC == iter->second[1];
  }
}

void LLVMBranchPredictor::update(const LLVMDynamicInst *inst) {
  auto pc = inst->getPC();
  auto dynamicNextPC = inst->getDynamicNextPC();
  auto iter = this->records.find(pc);
  if (iter == this->records.end()) {
    // This is a new inst.
    this->records.emplace(
        pc, std::array<uint64_t, 2>{dynamicNextPC, dynamicNextPC});
    return;
  } else {
    if (dynamicNextPC == iter->second[0]) {
      // We have two consecutive same target, update.
      iter->second[1] = iter->second[0];
    } else {
      // The target is different than previous one,
      if (dynamicNextPC != iter->second[1] &&
          iter->second[0] != iter->second[1]) {
        // We predict wrong twice, clear the prediction.
        iter->second[1] = 0;
      }
      // Update the previous one.
      iter->second[0] = dynamicNextPC;
    }
  }
}
} // namespace gem5

