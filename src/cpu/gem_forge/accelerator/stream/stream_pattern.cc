#include "stream_pattern.hh"

#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream pattern"
#endif

// #include "base/misc.hh""
#include "base/logging.hh"
#include "base/trace.hh"
#include "proto/protoio.hh"

namespace gem5 {

StreamPattern::StreamPattern(const std::string &_patternPath) {
  ProtoInputStream patternStream(_patternPath);
  LLVM::TDG::StreamPattern pattern;
  while (patternStream.read(pattern)) {
    this->patterns.push_back(pattern);
  }
  this->nextPattern = this->patterns.cbegin();
}

void StreamPattern::configure() {
  if (this->nextPattern == this->patterns.cend()) {
    panic("Failed to read in the next pattern from file.");
  }
  this->currentPattern = this->nextPattern;
  ++this->nextPattern;
}
} // namespace gem5
