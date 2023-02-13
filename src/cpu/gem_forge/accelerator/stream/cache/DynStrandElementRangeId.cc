#include "DynStrandElementRangeId.hh"

namespace gem5 {

std::ostream &operator<<(std::ostream &os, const DynStrandElementRangeId &id) {
  os << id.strandId << id.lhsElementIdx << '+'
     << id.rhsElementIdx - id.lhsElementIdx << '-';
  return os;
}} // namespace gem5

