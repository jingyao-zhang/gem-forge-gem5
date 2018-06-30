#ifndef __CPU_LLVM_INST_PARSER_HH__
#define __CPU_LLVM_INST_PARSER_HH__

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse llvm instructions."
#endif

#include "TDGInstruction.pb.h"
#include "llvm_accelerator.hh"
#include "llvm_insts.hh"

#include "proto/protoio.hh"

class LLVMInstParser {
public:
  LLVMInstParser(const std::string &fileName);

  /**
   * Parse llvm dynamic instructions.
   * Append to the buffer.
   * return the number of instructions parsed.
   */
  size_t parse(std::list<std::shared_ptr<LLVMDynamicInst>> &buffer);

private:
  ProtoInputStream *input;
  LLVM::TDG::TDG TDG;
};

#endif