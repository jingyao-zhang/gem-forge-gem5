#include "ExplicitReuseState.hh"

namespace gem5 {

std::ostream &operator<<(std::ostream &os, const ExplicitReuseState &r) {
  os << "ExplicitReuse " << r.current << '/' << r.total;
  return os;
}

std::string to_string(const ExplicitReuseState &r) {
  std::stringstream ss;
  ss << r;
  return ss.str();
}
}