#include "DynamicStreamId.hh"

#include <sstream>

std::ostream &operator<<(std::ostream &os, const DynamicStreamId &streamId) {
  os << streamId.streamName << " -" << streamId.coreId << '-'
     << streamId.staticId << '-' << streamId.streamInstance << "-";
  return os;
}

std::string to_string(const DynamicStreamId &streamId) {
  std::ostringstream ss;
  ss << streamId;
  return ss.str();
}
