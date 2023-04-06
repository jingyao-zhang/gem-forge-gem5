#include "stream_dep_edge.hh"

#include <cassert>

namespace gem5 {

const char *DynStreamDepEdge::typeToString(const TypeE &type) {
#define Case(x)                                                                \
  case x:                                                                      \
    return #x
  switch (type) {
    Case(Addr);
    Case(Value);
    Case(Back);
    Case(Pred);
    Case(Bound);
#undef Case
  default:
    assert(false && "Invalid StreamDepEdgeType.");
  }
}

std::ostream &operator<<(std::ostream &os,
                         const DynStreamDepEdge::TypeE &type) {
  os << DynStreamDepEdge::typeToString(type);
  return os;
}

std::string to_string(const DynStreamDepEdge::TypeE &type) {
  return std::string(DynStreamDepEdge::typeToString(type));
}

uint64_t DynStreamDepEdge::getBaseElemIdx(uint64_t elemIdx) const {
  uint64_t ret = this->alignBaseElement;
  if (this->baseElemSkipCnt != 0) {
    assert(this->baseElemReuseCnt == 1);
    ret += (elemIdx + 1) * this->baseElemSkipCnt;
  } else if (this->baseElemReuseCnt != 0) {
    ret += elemIdx / this->baseElemReuseCnt;
  }
  return ret;
}

}