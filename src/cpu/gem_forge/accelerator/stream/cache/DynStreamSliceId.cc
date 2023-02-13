#include "DynStreamSliceId.hh"

namespace gem5 {

std::ostream &operator<<(std::ostream &os, const DynStreamSliceId &id) {
  return os << id.elementRange;
}} // namespace gem5

