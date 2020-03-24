
#include "DynamicStreamSliceIdVec.hh"

std::ostream &operator<<(std::ostream &os,
                         const DynamicStreamSliceIdVec &slices) {
  for (const auto &sliceId : slices.sliceIds) {
    os << sliceId << ' ';
  }
  return os;
}