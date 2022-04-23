#include "AffinePattern.hh"

#include "base/trace.hh"

#include "debug/StreamPUM.hh"

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

AffinePattern
AffinePattern::removeReuseInSubRegion(const IntVecT &arraySizes,
                                      const AffinePattern &pattern) {
  ParamVecT fixed_params;
  int64_t accArraySize = 1;
  for (int dim = 0; dim < pattern.params.size(); ++dim) {
    const auto &p = pattern.params.at(dim);
    auto stride = (p.stride == 0) ? accArraySize : p.stride;
    auto trip = (p.stride == 0) ? 1 : p.trip;
    fixed_params.emplace_back(stride, trip);
    accArraySize *= arraySizes.at(dim);
  }
  return AffinePattern(pattern.start, fixed_params);
}

AffinePattern AffinePattern::intersectSubRegions(const IntVecT &array_sizes,
                                                 const AffinePattern &region1,
                                                 const AffinePattern &region2) {

  if (!region1.isSubRegionToArraySize(array_sizes)) {
    panic("Region1 %s Not SubRegion in Array %s.", region1, array_sizes);
  }

  if (!region2.isSubRegionToArraySize(array_sizes)) {
    panic("Region2 %s Not SubRegion in Array %s.", region2, array_sizes);
  }

  auto starts1 = region1.getSubRegionStartToArraySize(array_sizes);
  auto trips1 = region1.getTrips();
  auto starts2 = region2.getSubRegionStartToArraySize(array_sizes);
  auto trips2 = region2.getTrips();
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
  return constructSubRegion(array_sizes, intersect_starts, intersect_trips);
}

AffinePattern AffinePattern::unionSubRegions(const IntVecT &array_sizes,
                                             const AffinePattern &region1,
                                             const AffinePattern &region2) {

  if (!region1.isSubRegionToArraySize(array_sizes)) {
    panic("Region1 %s Not SubRegion in Array %s.", region1, array_sizes);
  }

  if (!region2.isSubRegionToArraySize(array_sizes)) {
    panic("Region2 %s Not SubRegion in Array %s.", region2, array_sizes);
  }

  auto starts1 = region1.getSubRegionStartToArraySize(array_sizes);
  auto trips1 = region1.getTrips();
  auto starts2 = region2.getSubRegionStartToArraySize(array_sizes);
  auto trips2 = region2.getTrips();
  IntVecT union_starts;
  IntVecT union_trips;
  auto dimension = array_sizes.size();
  for (auto i = 0; i < dimension; ++i) {
    auto s1 = starts1[i];
    auto t1 = trips1[i];
    auto s2 = starts2[i];
    auto t2 = trips2[i];
    auto ss = std::min(s1, s2);
    auto tt = std::max(s1 + t1, s2 + t2) - ss;
    union_starts.push_back(ss);
    union_trips.push_back(tt);
  }
  return constructSubRegion(array_sizes, union_starts, union_trips);
}

bool AffinePattern::isSubRegionToArraySize(const IntVecT &array_sizes,
                                           bool allow_reuse) const {
  auto dimension = array_sizes.size();
  if (params.size() != dimension) {
    return false;
  }
  // This is S1x... xSi
  IntVecT inner_array_sizes;
  for (auto i = 0; i < dimension; ++i) {
    auto s = reduce_mul(array_sizes.cbegin(), array_sizes.cbegin() + i, 1);
    inner_array_sizes.push_back(s);
  }

  auto strides = get_strides();
  auto trips = getTrips();
  for (auto i = 0; i < dimension; ++i) {
    auto s = strides[i];
    auto t = inner_array_sizes[i];
    if (s == t) {
      continue;
    }
    if (allow_reuse && s == 0) {
      continue;
    }
    return false;
  }
  auto starts = getSubRegionStartToArraySize(array_sizes);
  for (auto i = 0; i < dimension; ++i) {
    auto r = strides[i];
    if (r == 0) {
      // This is a reuse dimension.
      assert(allow_reuse && "Unreachable if not allow reuse.");
      continue;
    }
    auto p = starts[i];
    auto q = trips[i];
    auto s = array_sizes[i];
    if (p + q > s) {
      DPRINTF(StreamPUM,
              "[PUM] Not SubRegion as Overflow in Dim %d Start %ld Trips %ld "
              "Stride %ld ArraySize %ld.\n",
              i, p, q, r, s);
      return false;
    }
  }
  return true;
}

AffinePattern AffinePattern::splitFromDim(int64_t dim) {

  assert(dim < this->params.size());

  AffinePattern::ParamVecT outerDims;
  for (int i = dim; i < this->params.size(); ++i) {
    outerDims.push_back(this->params.at(i));
  }
  for (int i = dim; i < this->params.size(); ++i) {
    this->params.pop_back();
  }
  AffinePattern splitPat(0, outerDims);
  return splitPat;
}