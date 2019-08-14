#include "DynamicStreamId.hh"

std::ostream &operator<<(std::ostream &os, const DynamicStreamId &streamId) {
  os << "[Core " << streamId.coreId << "][" << streamId.name << "]";
  return os;
}