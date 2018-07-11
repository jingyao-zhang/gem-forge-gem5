#include "cpu/llvm_trace/llvm_branch_predictor.hh"
#include "base/trace.hh"
#include "debug/LLVMBranchPredictor.hh"

bool LLVMBranchPredictor::predictAndUpdate(
    const  LLVMDynamicInst* inst) {
  if (!inst->isConditionalBranchInst()) {
    panic("This is not a conditional branch inst %s.\n",
          inst->getInstName().c_str());
  }
  bool result = this->predict(inst);
  this->update(inst);

  DPRINTF(LLVMBranchPredictor, "Predict to inst %p, %s, result %d.\n",
          reinterpret_cast<void*>(inst->getStaticInstAddress()),
          inst->getInstName().c_str(), result);
  return result;
}

bool LLVMBranchPredictor::predict(
    const  LLVMDynamicInst* inst) const {
  uint64_t staticInstAddress = inst->getStaticInstAddress();
  const std::string& nextBBName = inst->getNextBBName();
  auto iter = this->records.find(staticInstAddress);
  if (iter == this->records.end()) {
    // This is a new inst, always predict wrong?
    return false;
  } else {
    // Check if we predict it right.
    return nextBBName == iter->second[1];
  }
}

void LLVMBranchPredictor::update(const  LLVMDynamicInst* inst) {
  uint64_t staticInstAddress = inst->getStaticInstAddress();
  const std::string& nextBBName = inst->getNextBBName();
  auto iter = this->records.find(staticInstAddress);
  if (iter == this->records.end()) {
    // This is a new inst.
    this->records[staticInstAddress] =
        std::array<std::string, 2>{nextBBName, nextBBName};
    return;
  } else {
    if (nextBBName == iter->second[0]) {
      // We have two consecutive same target, update.
      iter->second[1] = iter->second[0];
    } else {
      // The target is different than previous one,
      // just update the previous one but not update the result.
      if (nextBBName != iter->second[1] && iter->second[0] != iter->second[1]) {
        // We predict wrong twice.
        // Clear the prediction.
        iter->second[1] = "";
      }
      iter->second[0] = nextBBName;
    }
  }
}