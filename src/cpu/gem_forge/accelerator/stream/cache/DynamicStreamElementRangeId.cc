#include "DynamicStreamElementRangeId.hh"

std::ostream &operator<<(std::ostream &os,
                         const DynamicStreamElementRangeId &id) {
  os << id.streamId << id.lhsElementIdx << '+'
     << id.rhsElementIdx - id.lhsElementIdx << '-';
  return os;
}