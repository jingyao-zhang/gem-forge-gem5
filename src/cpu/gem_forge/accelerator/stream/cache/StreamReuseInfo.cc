#include "StreamReuseInfo.hh"

#include <sstream>

namespace gem5 {

std::ostream &operator<<(std::ostream &os, const StreamReuseInfo &info) {
  os << info.reuseTileSize << 'x' << info.reuseCount << '@' << info.reuseDim
     << '-' << info.reuseDimEnd;
  return os;
}

std::string to_string(const StreamReuseInfo &info) {
  std::ostringstream ss;
  ss << info;
  return ss.str();
}
} // namespace gem5