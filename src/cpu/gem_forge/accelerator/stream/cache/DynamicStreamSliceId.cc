#include "DynamicStreamSliceId.hh"

std::ostream &operator<<(std::ostream &os, const DynamicStreamSliceId &id) {
  os << "[Core " << id.streamId.coreId << "][" << id.streamId.streamName << "]["
     << id.lhsElementIdx << ", " << id.rhsElementIdx << ")";
  return os;
}