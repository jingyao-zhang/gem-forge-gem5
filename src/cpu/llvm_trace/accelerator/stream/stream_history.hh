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

class StreamHistory {
public:
  StreamHistory(const std::string &_historyPath);

  /**
   * Read the next history entry from the stream.
   */
  void configure();

  /**
   * Return the next value of the history.
   * The first boolean indicating the value is valid.
   */
  std::pair<bool, uint64_t> getNextAddr(bool& used);

private:
  std::string historyPath;
  ProtoInputStream historyStream;
  LLVM::TDG::StreamHistory history;

  size_t currentIdx;
  uint64_t previousAddr;
};

#endif