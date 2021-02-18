#include "DynamicStreamSliceId.hh"

std::ostream &operator<<(std::ostream &os, const DynamicStreamSliceId &id) {
  return os << id.elementRange;
}