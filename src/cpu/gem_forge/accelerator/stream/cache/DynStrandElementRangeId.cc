#include "DynStrandElementRangeId.hh"

std::ostream &operator<<(std::ostream &os, const DynStrandElementRangeId &id) {
  os << id.strandId << id.lhsElementIdx << '+'
     << id.rhsElementIdx - id.lhsElementIdx << '-';
  return os;
}