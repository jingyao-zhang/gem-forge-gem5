
#include "DynStreamSliceIdVec.hh"

namespace gem5 {

std::ostream &operator<<(std::ostream &os, const DynStreamSliceIdVec &slices) {
  os << '[';
  for (const auto &sliceId : slices.sliceIds) {
    os << ' ' << sliceId;
  }
  os << ']';
  return os;
}} // namespace gem5

