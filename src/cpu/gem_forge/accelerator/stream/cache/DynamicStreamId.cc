#include "DynamicStreamId.hh"

std::ostream &operator<<(std::ostream &os, const DynamicStreamId &streamId) {
  os << "[" << streamId.coreId << '-' << streamId.staticId << '-'
     << streamId.streamInstance << "][" << streamId.streamName << "]";
  return os;
}