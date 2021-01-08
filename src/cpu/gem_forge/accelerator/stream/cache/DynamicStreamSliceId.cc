#include "DynamicStreamSliceId.hh"

std::ostream &operator<<(std::ostream &os, const DynamicStreamSliceId &id) {
  os << id.streamId << id.lhsElementIdx << '+'
     << id.rhsElementIdx - id.lhsElementIdx << '-';
  return os;
}