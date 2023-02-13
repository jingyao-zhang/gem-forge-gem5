#ifndef __CPU_TDG_ACCELERATOR_STREAM_PATTERN_H__
#define __CPU_TDG_ACCELERATOR_STREAM_PATTERN_H__

#include "StreamMessage.pb.h"

#include <list>
#include <string>

namespace gem5 {

class StreamPattern {
public:
  StreamPattern(const std::string &_patternPath);

  void configure();

  const LLVM::TDG::StreamPattern &getPattern() const {
    return *this->currentPattern;
  }

private:
  using PatternList = std::list<LLVM::TDG::StreamPattern>;
  PatternList patterns;

  PatternList::const_iterator nextPattern;
  PatternList::const_iterator currentPattern;
};

} // namespace gem5

#endif