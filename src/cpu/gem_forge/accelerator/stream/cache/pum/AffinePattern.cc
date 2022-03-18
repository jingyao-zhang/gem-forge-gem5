#include "AffinePattern.hh"

#include "base/trace.hh"

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

std::ostream &operator<<(std::ostream &os,
                         const AffinePattern::IntVecT &intVec) {
  for (const auto &v : intVec) {
    os << 'x' << v;
  }
  return os;
}

AffinePattern AffinePattern::intersectSubRegions(const IntVecT &array_sizes,
                                                 const AffinePattern &region1,
                                                 const AffinePattern &region2) {

  if (!region1.is_canonical_sub_region_to_array_size(array_sizes)) {
    panic("Region1 %s Not SubRegion in Array %s.", region1, array_sizes);
  }

  if (!region2.is_canonical_sub_region_to_array_size(array_sizes)) {
    panic("Region2 %s Not SubRegion in Array %s.", region2, array_sizes);
  }

  auto starts1 = region1.getSubRegionStartToArraySize(array_sizes);
  auto trips1 = region1.get_trips();
  auto starts2 = region2.getSubRegionStartToArraySize(array_sizes);
  auto trips2 = region2.get_trips();
  IntVecT intersect_starts;
  IntVecT intersect_trips;
  auto dimension = array_sizes.size();
  for (auto i = 0; i < dimension; ++i) {
    auto s1 = starts1[i];
    auto t1 = trips1[i];
    auto s2 = starts2[i];
    auto t2 = trips2[i];
    if (s1 >= s2 + t2 || s2 >= s1 + t1) {
      // None means empty intersection.
      int64_t start = 0;
      ParamVecT params;
      params.emplace_back(1, 0);
      return AffinePattern(start, params);
    }
    auto ss = std::max(s1, s2);
    auto tt = std::min(s1 + t1, s2 + t2) - ss;
    intersect_starts.push_back(ss);
    intersect_trips.push_back(tt);
  }
  return construct_canonical_sub_region(array_sizes, intersect_starts,
                                        intersect_trips);
}