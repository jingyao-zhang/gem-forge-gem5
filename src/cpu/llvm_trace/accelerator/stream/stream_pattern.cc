#include "stream_pattern.hh"

#include "base/misc.hh"
#include "base/trace.hh"

StreamPattern::StreamPattern(const std::string &_patternPath)
    : patternPath(_patternPath), patternStream(_patternPath) {}

void StreamPattern::configure() {
  if (!this->patternStream.read(this->pattern)) {
    panic("Failed to read in the next pattern from file %s.",
          this->patternPath.c_str());
  }
}
