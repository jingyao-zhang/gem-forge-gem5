
#include "DynamicStreamSliceIdVec.hh"

std::ostream &operator<<(std::ostream &os,
                         const DynamicStreamSliceIdVec &slices) {
  os << '[';
  for (const auto &sliceId : slices.sliceIds) {
    os << ' ' << sliceId;
  }
  os << ']';
  return os;
}