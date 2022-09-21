#include "AffinePattern.hh"

#include "base/trace.hh"
#include "debug/MLCStreamPUM.hh"

const size_t AffinePattern::MaxDimension;

AffinePattern::AffinePattern(::LLVM::TDG::AffinePattern tdgAffinePattern) {
  this->start = tdgAffinePattern.start();
  for (const auto &p : tdgAffinePattern.params()) {
    this->params.push_back({p.stride(), p.tc()});
  }
}

::LLVM::TDG::AffinePattern AffinePattern::toTDGAffinePattern() const {
  ::LLVM::TDG::AffinePattern tdgAffinePattern;
  tdgAffinePattern.set_start(this->start);
  for (const auto &param : this->params) {
    auto tdgParam = tdgAffinePattern.add_params();
    tdgParam->set_stride(param.stride);
    tdgParam->set_tc(param.trip);
  }
  return tdgAffinePattern;
}

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

bool AffinePattern::canContractCanonicalTileToDim(int64_t targetDim) const {
  auto dimension = this->getCanonicalTileDim();
  if (dimension < targetDim) {
    return false;
  }
  if (dimension == targetDim) {
    return true;
  }
  /**
   * Contractable if all TileSize higher than TargetDim is 1.
   */
  for (auto i = targetDim; i < dimension; ++i) {
    if (this->params[i].trip != 1) {
      return false;
    }
  }
  return true;
}

AffinePattern
AffinePattern::contractCanonicalTileToDim(int64_t targetDim) const {

  assert(this->canContractCanonicalTileToDim(targetDim));

  auto dimension = this->getCanonicalTileDim();

  auto tileArraySize = this->getTileAndArraySize();
  auto &tileSizes = tileArraySize.first;
  auto &arraySizes = tileArraySize.second;

  // Contract the tile sizes.
  tileSizes.resize(targetDim);

  // Adjust the array sizes.
  for (int i = targetDim; i < dimension; ++i) {
    arraySizes.at(targetDim - 1) *= arraySizes.at(i);
  }
  arraySizes.resize(targetDim);

  return AffinePattern::construct_canonical_tile(tileSizes, arraySizes);
}

AffinePattern::IntVecT
AffinePattern::getArrayPosition(const IntVecT &arraySizes, int64_t linearPos) {
  /**
    Given a linear position, return the position according to the array
    dimension.
   */
  auto dimension = arraySizes.size();

#define dispatch_impl(dim)                                                     \
  auto fixedArraySizes =                                                       \
      AffinePatternImpl<dim, int64_t>::getFixSizedIntVec(arraySizes);          \
  auto fixedArrayPos = AffinePatternImpl<dim, int64_t>::getArrayPosition(      \
      fixedArraySizes, linearPos);                                             \
  return AffinePatternImpl<dim, int64_t>::getHeapIntVec(fixedArrayPos);

  if (dimension == 1) {
    dispatch_impl(1);
  } else if (dimension == 2) {
    dispatch_impl(2);
  } else if (dimension == 3) {
    dispatch_impl(3);
  }

#undef dispatch_impl

  // This is S1x... xSi
  IntVecT inner_array_sizes(dimension);
  for (auto i = 0; i < dimension; ++i) {
    auto s = reduce_mul(arraySizes.cbegin(), arraySizes.cbegin() + i, 1);
    inner_array_sizes[i] = s;
  }
  IntVecT pos(dimension);
  int64_t cur_pos = std::abs(linearPos);
  for (int i = dimension - 1; i >= 0; --i) {
    auto p = cur_pos / inner_array_sizes[i];

    pos[i] = (linearPos > 0) ? p : -p;

    cur_pos = cur_pos % inner_array_sizes[i];
  }
  return pos;
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

#ifndef NDEBUG
  if (!region1.isSubRegionToArraySize(array_sizes)) {
    panic("Region1 %s Not SubRegion in Array %s.", region1, array_sizes);
  }

  if (!region2.isSubRegionToArraySize(array_sizes)) {
    panic("Region2 %s Not SubRegion in Array %s.", region2, array_sizes);
  }
#endif

#define dispatch_impl(dim)                                                     \
  auto fixedArraySizes =                                                       \
      AffinePatternImpl<dim, int64_t>::getFixSizedIntVec(array_sizes);         \
  auto fixedRegion1 = getAffinePatternImpl<dim, int64_t>(region1);             \
  auto fixedRegion2 = getAffinePatternImpl<dim, int64_t>(region2);             \
  auto fixedIntersect = AffinePatternImpl<dim, int64_t>::intersectSubRegions(  \
      fixedArraySizes, fixedRegion1, fixedRegion2);                            \
  return getAffinePatternFromImpl<dim, int64_t>(fixedIntersect);

  auto dimension = array_sizes.size();
  if (dimension == 1) {
    dispatch_impl(1);
  } else if (dimension == 2) {
    dispatch_impl(2);
  } else if (dimension == 3) {
    dispatch_impl(3);
  }

#undef dispatch_impl

  auto starts1 = region1.getSubRegionStartToArraySize(array_sizes);
  auto trips1 = region1.getTrips();
  auto starts2 = region2.getSubRegionStartToArraySize(array_sizes);
  auto trips2 = region2.getTrips();
  IntVecT intersect_starts;
  IntVecT intersect_trips;
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

#ifndef NDEBUG
  if (!region1.isSubRegionToArraySize(array_sizes)) {
    panic("Region1 %s Not SubRegion in Array %s.", region1, array_sizes);
  }

  if (!region2.isSubRegionToArraySize(array_sizes)) {
    panic("Region2 %s Not SubRegion in Array %s.", region2, array_sizes);
  }
#endif

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
      DPRINTF(MLCStreamPUM,
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
  assert(dim > 0);

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

void AffinePattern::mergeOutDim(const AffinePattern &splitOutDim) {
  if (splitOutDim.getTotalTrip() == 0) {
    return;
  }
  assert(splitOutDim.start == 0);
  for (const auto &p : splitOutDim.params) {
    this->params.push_back(p);
  }
}

std::vector<AffinePattern>
AffinePattern::break_continuous_range_into_canonical_sub_regions(
    const IntVecT &array_sizes, int64_t start, int64_t trip) {

#define dispatch_impl(dim)                                                     \
  {                                                                            \
    auto fixedArraySizes =                                                     \
        AffinePatternImpl<dim, int64_t>::getFixSizedIntVec(array_sizes);       \
    auto fixedSubRegions = AffinePatternImpl<dim, int64_t>::                   \
        breakContnuousRangeIntoSubRegionStartAndTrips(fixedArraySizes, start,  \
                                                      trip);                   \
    std::vector<AffinePattern> ret;                                            \
    for (auto i = 0; i < fixedSubRegions.count; ++i) {                         \
      const auto &subRegion = fixedSubRegions.subRegions.at(i);                \
      auto fixedAffPat = AffinePatternImpl<dim, int64_t>::constructSubRegion(  \
          fixedArraySizes, subRegion.starts, subRegion.trips);                 \
      ret.push_back(getAffinePatternFromImpl(fixedAffPat));                    \
    }                                                                          \
    return ret;                                                                \
  }

  auto dimension = array_sizes.size();
  if (dimension == 1) {
    dispatch_impl(1);
  } else if (dimension == 2) {
    dispatch_impl(2);
  } else if (dimension == 3) {
    dispatch_impl(3);
  } else {
    // Handle possible cases when start/trip overflow array_sizes.
    auto totalSize = reduce_mul(array_sizes.begin(), array_sizes.end(), 1);
    if (start >= totalSize || start + trip <= 0) {
      // No overlap at all.
      return std::vector<AffinePattern>();
    }
    trip = std::min(trip, totalSize - start);
    auto ps = getArrayPosition(array_sizes, start);
    auto qs = getArrayPosition(array_sizes, start + trip);
    return recursive_break_continuous_range_into_canonical_sub_regions(
        array_sizes, ps, qs, 0 /* dim */);
  }

#undef dispatch_impl
}

std::vector<AffinePattern>
AffinePattern::recursive_break_continuous_range_into_canonical_sub_regions(
    const IntVecT &array_sizes, IntVecT &ps, IntVecT &qs, int64_t dim) {
  /**
  This method breaks a continuous range [start, start + trip) into a list of
  sub regions. Specifically, it aligns the start and and to each dimension
  by creating new sub regions if the mod is not zero.

  |--------|-------|-------|-------|-------|-------|-------|--------|
           A   P   B       C   Q   D

  Create a sub region for [P, B), [C, Q) and keep [B, C) continuous.
  */
  std::vector<AffinePattern> sub_regions;
  auto dimension = array_sizes.size();

  auto p = ps[dim];
  auto q = qs[dim];
  auto t = array_sizes[dim];

  // print(f'dim={dim} array={array_sizes} ps={ps} qs={qs}')

  bool high_dim_match = true;
  for (auto i = dim + 1; i < dimension; ++i) {
    if (ps[i] != qs[i]) {
      high_dim_match = false;
      break;
    }
  }

  if (p != 0 && q != 0 && high_dim_match) {
    // One sub region [P, Q)
    IntVecT starts(ps);
    IntVecT trips(array_sizes);
    for (auto i = dim; i < dimension; ++i) {
      trips[i] = 1;
    }
    trips[dim] = q - p;
    sub_regions.push_back(constructSubRegion(array_sizes, starts, trips));
  } else {
    if (p != 0) {
      // One sub region [P, B)
      IntVecT starts(ps);
      IntVecT trips(array_sizes);
      for (auto i = dim; i < dimension; ++i) {
        trips[i] = 1;
      }
      trips[dim] = t - p;
      sub_regions.push_back(constructSubRegion(array_sizes, starts, trips));
    }

    if (q != 0) {
      IntVecT starts(qs);
      starts[dim] = 0;
      IntVecT trips(array_sizes);
      for (auto i = dim; i < dimension; ++i) {
        trips[i] = 1;
      }
      trips[dim] = q;
      sub_regions.push_back(constructSubRegion(array_sizes, starts, trips));
    }

    if (!high_dim_match) {
      // There is more to match.
      assert(dim + 1 < dimension);
      IntVecT bs(ps);
      if (p != 0) {
        bs[dim] = 0;
        bs[dim + 1] += 1;
        // Adjust starting point if we need to carry.
        for (auto i = dim + 1; i < dimension - 1; ++i) {
          if (bs[i] == array_sizes[i]) {
            bs[i] = 0;
            bs[i + 1] += 1;
          }
        }
      }
      IntVecT cs(qs);
      if (q != 0) {
        cs[dim] = 0;
      }
      bool bs_eq_cs = true;
      for (auto i = 0; i < dimension; ++i) {
        auto b = bs[i];
        auto c = cs[i];
        if (b != c) {
          bs_eq_cs = false;
          break;
        }
      }
      if (!bs_eq_cs) {
        auto ret = recursive_break_continuous_range_into_canonical_sub_regions(
            array_sizes, bs, cs, dim + 1);
        sub_regions.insert(sub_regions.end(), ret.begin(), ret.end());
      }
    }
  }
  return sub_regions;
}