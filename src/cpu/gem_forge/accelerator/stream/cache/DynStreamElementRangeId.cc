#include "DynStreamElementRangeId.hh"

std::ostream &operator<<(std::ostream &os,
                         const DynStreamElementRangeId &id) {
  os << id.streamId << id.lhsElementIdx << '+'
     << id.rhsElementIdx - id.lhsElementIdx << '-';
  return os;
}