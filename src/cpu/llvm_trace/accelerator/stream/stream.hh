#ifndef __CPU_TDG_ACCELERATOR_STREAM_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_HH__

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse llvm instructions."
#endif

#include "cpu/llvm_trace/TDGInstruction.pb.h"
#include "cpu/llvm_trace/accelerator/stream/StreamMessage.pb.h"

#include "proto/protoio.hh"

class Stream {
public:
  Stream(const LLVM::TDG::TDGInstruction &configInst);

  uint64_t getStreamId() const { return this->info.id(); }
  const std::string &getStringName() const { return this->info.name(); }

private:
  LLVM::TDG::StreamInfo info;
};

#endif