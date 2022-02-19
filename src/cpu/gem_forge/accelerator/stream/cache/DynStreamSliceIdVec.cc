
#include "DynStreamSliceIdVec.hh"

std::ostream &operator<<(std::ostream &os, const DynStreamSliceIdVec &slices) {
  os << '[';
  for (const auto &sliceId : slices.sliceIds) {
    os << ' ' << sliceId;
  }
  os << ']';
  return os;
}