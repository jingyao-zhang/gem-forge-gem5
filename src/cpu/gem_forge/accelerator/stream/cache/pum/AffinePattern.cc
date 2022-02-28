#include "AffinePattern.hh"

std::ostream &operator<<(std::ostream &os, const AffinePattern &pattern) {
  os << pattern.start;
  for (const auto &p : pattern.params) {
    os << ':' << p.stride << ':' << p.trip;
  }
  return os;
}

std::string AffinePattern::to_string() const {
  std::stringstream os;
  os << *this;
  return os.str();
}

AffinePattern AffinePattern::parse(const std::string &s) {
  IntVecT p;
  size_t cur_pos = 0;
  auto next_pos = s.find(':', cur_pos);
  while (next_pos != s.npos) {
    auto substr = s.substr(cur_pos, next_pos - cur_pos);
    auto v = std::stol(substr);
    p.push_back(v);
    cur_pos = next_pos + 1;
    next_pos = s.find(':', cur_pos);
  }
  // Handle the last value.
  auto substr = s.substr(cur_pos);
  auto v = std::stol(substr);
  p.push_back(v);

  assert(p.size() % 2 == 1);
  auto start = p[0];
  ParamVecT params;
  for (auto i = 1; i < p.size(); i += 2) {
    params.emplace_back(p[i], p[i + 1]);
  }
  return AffinePattern(start, params);
}