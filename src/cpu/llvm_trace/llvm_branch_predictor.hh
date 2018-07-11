#ifndef __CPU_LLVM_BRANCH_PREDICTOR_HH__
#define __CPU_LLVM_BRANCH_PREDICTOR_HH__

#include "cpu/llvm_trace/llvm_insts.hh"

#include <array>
#include <string>
#include <unordered_map>

// A simple local 2 bit predictor.
class LLVMBranchPredictor {
public:
  virtual ~LLVMBranchPredictor() {}

  // Return true if the predictor predicts it right.
  // False otherwise.
  // This will also update the predictor.
  virtual bool predictAndUpdate(const LLVMDynamicInst *inst);

protected:
  std::unordered_map<uint64_t, std::array<std::string, 2>> records;

  bool predict(const LLVMDynamicInst *inst) const;
  void update(const LLVMDynamicInst *inst);
};

#endif