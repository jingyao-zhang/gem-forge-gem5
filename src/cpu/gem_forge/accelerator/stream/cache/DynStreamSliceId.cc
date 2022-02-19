#include "DynStreamSliceId.hh"

std::ostream &operator<<(std::ostream &os, const DynStreamSliceId &id) {
  return os << id.elementRange;
}