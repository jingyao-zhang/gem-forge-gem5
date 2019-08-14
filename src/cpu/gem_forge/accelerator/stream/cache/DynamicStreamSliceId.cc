#include "DynamicStreamSliceId.hh"

std::ostream &operator<<(std::ostream &os, const DynamicStreamSliceId &id) {
  os << "[Core " << id.streamId.coreId << "][" << id.streamId.name << "]["
     << id.startIdx << ", " << id.endIdx << ")";
  return os;
}