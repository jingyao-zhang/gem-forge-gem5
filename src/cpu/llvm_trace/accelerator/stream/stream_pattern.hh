#ifndef __CPU_TDG_ACCELERATOR_STREAM_PATTERN_H__
#define __CPU_TDG_ACCELERATOR_STREAM_PATTERN_H__

#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream pattern"
#endif

#include "StreamMessage.pb.h"

#include "proto/protoio.hh"

#include <string>

class StreamPattern {
public:
  StreamPattern(const std::string &_patternPath);

  void configure();

  const LLVM::TDG::StreamPattern &getPattern() const { return this->pattern; }

private:
  std::string patternPath;
  ProtoInputStream patternStream;
  LLVM::TDG::StreamPattern pattern;
};

#endif