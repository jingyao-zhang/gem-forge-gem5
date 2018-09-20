#include "stream_pattern.hh"

#include "base/misc.hh"
#include "base/trace.hh"
#include "debug/StreamEngine.hh"

StreamPattern::StreamPattern(const std::string &_patternPath)
    : patternPath(_patternPath), patternStream(_patternPath) {}

void StreamPattern::configure() {
  if (!this->patternStream.read(this->pattern)) {
    panic("Failed to read in the next pattern from file %s.",
          this->patternPath.c_str());
  }

  this->idxI = 0;
  this->idxJ = 0;
}

std::pair<bool, uint64_t> StreamPattern::getNextValue() {
  if (this->pattern.val_pattern() == "CONSTANT") {
    return std::make_pair(true, this->pattern.base());
  }

  if (this->pattern.val_pattern() == "UNKNOWN") {
    // Jesus.
    return std::make_pair(false, static_cast<uint64_t>(0));
  }

  if (this->pattern.val_pattern() == "LINEAR") {
    uint64_t nextValue =
        this->pattern.base() + this->pattern.stride_i() * this->idxI;
    this->idxI++;
    return std::make_pair(true, nextValue);
  }

  if (this->pattern.val_pattern() == "QUARDRIC") {
    uint64_t nextValue = this->pattern.base() +
                         this->pattern.stride_i() * this->idxI +
                         this->pattern.stride_j() * this->idxJ;
    this->idxI++;
    if (this->idxI == this->pattern.ni()) {
      this->idxI = 0;
      this->idxJ++;
    }
    return std::make_pair(true, nextValue);
  }

  if (this->pattern.val_pattern() == "RANDOM") {
    // Jesus.
    return std::make_pair(false, static_cast<uint64_t>(0));
  }

  panic("Unknown value pattern %s.", this->pattern.val_pattern().c_str());
}