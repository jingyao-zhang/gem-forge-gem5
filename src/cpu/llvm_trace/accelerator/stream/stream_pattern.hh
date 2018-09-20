#ifndef __CPU_TDG_ACCELERATOR_STREAM_PATTERN_H__
#define __CPU_TDG_ACCELERATOR_STREAM_PATTERN_H__

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream pattern."
#endif

#include "cpu/llvm_trace/accelerator/stream/StreamMessage.pb.h"

#include "proto/protoio.hh"

#include <string>

class StreamPattern {
public:
  StreamPattern(const std::string &_patternPath);

  /**
   * Read the next pattern from the stream.
   */
  void configure();

  /**
   * Return the next value of the pattern.
   * The first boolean indicating the value is valid.
   * Since we do not actually introduce the ability of compute indirect address,
   * for random pattern we do not know the missing address if it does not show
   * up in the trace.
   */
  std::pair<bool, uint64_t> getNextValue();

private:
  std::string patternPath;
  ProtoInputStream patternStream;
  LLVM::TDG::StreamPattern pattern;

  /**
   * Used for the linear and quardric pattern.
   */
  uint64_t idxI;
  uint64_t idxJ;
};

#endif