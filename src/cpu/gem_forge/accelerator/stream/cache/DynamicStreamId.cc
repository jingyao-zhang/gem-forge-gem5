#include "DynamicStreamId.hh"

std::ostream &operator<<(std::ostream &os, const DynamicStreamId &streamId) {
  os << streamId.streamName << " -" << streamId.coreId << '-'
     << streamId.staticId << '-' << streamId.streamInstance << "-";
  return os;
}