#include "DynStrandId.hh"

#include <sstream>

std::ostream &operator<<(std::ostream &os, const DynStrandId &strandId) {
  if (strandId.totalStrands > 1) {
    os << strandId.dynStreamId << strandId.strandIdx << '/'
       << strandId.totalStrands << '-';
  } else {
    // Ignore StrandIdx if there is only one strand.
    os << strandId.dynStreamId;
  }
  return os;
}

std::string to_string(const DynStrandId &strandId) {
  std::ostringstream ss;
  ss << strandId;
  return ss.str();
}
