#ifndef __CPU_GEM_FORGE_AFFINE_PATTERN_HH__
#define __CPU_GEM_FORGE_AFFINE_PATTERN_HH__

#include "base/types.hh"

#include <cstdlib>
#include <sstream>
#include <tuple>
#include <vector>

class AffinePattern {
public:
  /**
    This represents an affine pattern:
    start : stride1 : trip1 : ... : stride_n : trip_n

    The formula:
        start +
          (i % trip1) * stride1 +
          ((i / (trip1)) % trip2) * stride2 + ... +
          ((i / (trip1 x ... x trip_{n-1})) % trip_n) * stride_n
   */

  struct Param {
    int64_t stride;
    int64_t trip;
    Param(int _stride, int _trip) : stride(_stride), trip(_trip) {}
  };
  using ParamVecT = std::vector<Param>;
  using IntVecT = std::vector<int64_t>;

  int64_t start;
  ParamVecT params;

  // Default create an empty pattern.
  AffinePattern() : start(0), params(1, Param(1, 0)) {}
  AffinePattern(int64_t _start, ParamVecT _params)
      : start(_start), params(std::move(_params)) {}

  int64_t operator()(int64_t i) const {
    int64_t result = start;
    int64_t accTrip = 1;
    for (const auto &p : params) {
      result += p.stride * ((i / accTrip) % p.trip);
      accTrip *= p.trip;
    }
    return result;
  }

  bool operator==(const AffinePattern &other) const {
    if (start != other.start) {
      return false;
    }
    if (params.size() != other.params.size()) {
      return false;
    }
    for (auto i = 0; i < params.size(); ++i) {
      if (params[i].stride != other.params[i].stride) {
        return false;
      }
      if (params[i].trip != other.params[i].trip) {
        return false;
      }
    }
    return true;
  }

  bool operator!=(const AffinePattern &other) const {
    return !this->operator==(other);
  }

  static int64_t reduce_mul(IntVecT::const_iterator s,
                            IntVecT::const_iterator t, int64_t init) {
    int64_t ret = init;
    while (s != t) {
      ret *= *s;
      ++s;
    }
    return ret;
  }

  /**
    This represents a canonical tiling pattern:
    1. No reuse (hence one-to-one mapping)
    2. Must be tiling form:
    First tiling for [T1 x T2 x ... x Tn], then span to the entire N
    dimention array of size (S1 x S2 x ... x Sn).

    Examples:
    1D: 0 : 1 : T1 : T1 : S1/T1
    2D: 0 : 1 : T1 : S1 : T2    : T1    : S1/T1 : T2xS1 : S2/T2
    3D: 0 : 1 : T1 : S1 : T2    : S1xS2 : T3    : T1    : S1/T1
        : T2xS1 : S2/T2 : T3xS1xS2 : S3/T3

    The formal definition:
        0 : 1 : T1 : S1 : T2 : ... : S1x...S_{n-1} : Tn
          : T1 : S1/T1 : ... : S1x...xS_{n-1}xTn : Sn/Tn

    Notice that some special cases can be rewritten into canonical form by
    inserting Tx=1 dimension. For example: Column major access of 2D array:
      Column-major:   0 : S1 : S2 : 1  : S1
      Canonical:      0 : 1  : 1  : S1 : S2 : 1 : S1 : S1xS2 : 1
      (T1 = 1, T2 = S2)

    Given a canonical tiling pattern, we can construct the reverse pattern via:
    1st dimension keep the same                  0 : 1 : T1
    2nd dimension move to the tile size            : T1xT2x...xTn : S1/T1
    3rd dimension move to the 2nd tile dimension   : T1 : T2
    4th dimension move to the S2/T2 tile           : S1xT2x...xTn : S2/T2
    5th dimension move to the 3rd tile dimension   : T1xT2 : T3
    6th dimension move to the S3/T3 tile           : S1xS2x...xTn : S3/T3

    TODO: How to support nested tiling?
   */
  bool is_canonical_tile() const {
    if (start != 0) {
      return false;
    }
    if (params.size() % 2 != 0) {
      return false;
    }
    if (params.empty()) {
      return false;
    }
    auto dimension = params.size() / 2;
    auto ret = getTileAndArraySize();
    const auto &tile_sizes = ret.first;
    const auto &array_sizes = ret.second;
    for (auto i = 0; i < dimension; ++i) {
      auto stride = params[i].stride;
      auto correct_stride =
          reduce_mul(array_sizes.cbegin(), array_sizes.cbegin() + i, 1);
      if (stride != correct_stride) {
        return false;
      }
    }
    for (auto i = 0; i < dimension; ++i) {
      auto stride = params[i + dimension].stride;
      auto correct_stride = reduce_mul(array_sizes.cbegin(),
                                       array_sizes.cbegin() + i, tile_sizes[i]);
      if (stride != correct_stride) {
        return false;
      }
    }
    return true;
  }

  std::pair<IntVecT, IntVecT> getTileAndArraySize() const {
    auto dimension = params.size() / 2;
    IntVecT tile_sizes;
    IntVecT array_sizes;
    for (auto i = 0; i < dimension; ++i) {
      tile_sizes.push_back(params[i].trip);
      array_sizes.push_back(params[i].trip * params[i + dimension].trip);
    }
    return std::make_pair<IntVecT, IntVecT>(std::move(tile_sizes),
                                            std::move(array_sizes));
  }

  int64_t getCanonicalTotalTileSize() const {
    auto dimension = params.size() / 2;
    auto totalTileSize = 1;
    for (auto i = 0; i < dimension; ++i) {
      totalTileSize *= params[i].trip;
    }
    return totalTileSize;
  }

  static AffinePattern construct_canonical_tile(IntVecT tile_sizes,
                                                IntVecT array_sizes) {
    assert(tile_sizes.size() == array_sizes.size());
    auto dimension = tile_sizes.size();
    for (auto i = 0; i < dimension; ++i) {
      auto t = tile_sizes[i];
      auto a = array_sizes[i];
      assert(a % t == 0);
      assert(a >= t);
    }
    int64_t start = 0;
    ParamVecT params;
    for (auto i = 0; i < dimension; ++i) {
      auto stride =
          reduce_mul(array_sizes.cbegin(), array_sizes.cbegin() + i, 1);
      params.emplace_back(stride, tile_sizes[i]);
    }
    for (auto i = 0; i < dimension; ++i) {
      auto stride = reduce_mul(array_sizes.cbegin(), array_sizes.cbegin() + i,
                               tile_sizes[i]);
      auto trip = array_sizes[i] / tile_sizes[i];
      params.emplace_back(stride, trip);
    }
    return AffinePattern(start, params);
  }

  AffinePattern revert_canonical_tile() const {
    assert(is_canonical_tile());
    auto dimension = params.size() / 2;

    auto ret = getTileAndArraySize();
    const auto &tile_sizes = ret.first;
    const auto &array_sizes = ret.second;

    int64_t start = 0;
    ParamVecT params;

    for (auto i = 0; i < dimension; ++i) {
      auto stride1 =
          reduce_mul(tile_sizes.cbegin(), tile_sizes.cbegin() + i, 1);
      params.emplace_back(stride1, tile_sizes[i]);

      int64_t stride2 = 1;
      for (auto j = 0; j < dimension; ++j) {
        if (j < i) {
          stride2 *= array_sizes[j];
        } else {
          stride2 *= tile_sizes[j];
        }
      }
      int64_t trip2 = array_sizes[i] / tile_sizes[i];
      params.emplace_back(stride2, trip2);
    }
    return AffinePattern(start, params);
  }

  IntVecT get_strides() const {
    IntVecT ret;
    for (const auto &p : params) {
      ret.push_back(p.stride);
    }
    return ret;
  }

  IntVecT get_trips() const {
    IntVecT ret;
    for (const auto &p : params) {
      ret.push_back(p.trip);
    }
    return ret;
  }

  int64_t get_total_trip() const {
    int64_t ret = 1;
    for (const auto &p : params) {
      ret *= p.trip;
    }
    return ret;
  }

  bool is_empty() const { return get_total_trip() == 0; }

  bool check_is_reverse(const AffinePattern &reverse) const {
    for (int64_t i = 0; i < get_total_trip(); ++i) {
      auto f = this->operator()(i);
      auto reverse_i = reverse(f);
      if (reverse_i != i) {
        return false;
      }
    }
    return true;
  }

  std::string to_string() const;

  static AffinePattern parse(const std::string &s);

  static IntVecT getArrayPosition(const IntVecT &arraySizes,
                                  int64_t linearPos) {
    /**
      Given a linear position, return the position according to the array
      dimension.
     */
    auto dimension = arraySizes.size();
    // This is S1x... xSi
    IntVecT inner_array_sizes;
    for (auto i = 0; i < dimension; ++i) {
      auto s = reduce_mul(arraySizes.cbegin(), arraySizes.cbegin() + i, 1);
      inner_array_sizes.push_back(s);
    }
    IntVecT pos;
    int64_t cur_pos = std::abs(linearPos);
    for (int i = dimension - 1; i >= 0; --i) {
      auto p = cur_pos / inner_array_sizes[i];

      if (linearPos > 0) {
        pos.insert(pos.begin(), p);
      } else {
        pos.insert(pos.begin(), -p);
      }
      cur_pos = cur_pos % inner_array_sizes[i];
    }
    return pos;
  }

  IntVecT getSubRegionStartToArraySize(const IntVecT &arraySizes) const {
    return getArrayPosition(arraySizes, start);
  }

  bool is_canonical_sub_region_to_array_size(const IntVecT &array_sizes,
                                             bool allow_reuse = false) const;

  static AffinePattern
  construct_canonical_sub_region(const IntVecT &array_sizes,
                                 const IntVecT &starts, const IntVecT &trips) {
    auto dimension = array_sizes.size();
    assert(starts.size() == dimension);
    assert(trips.size() == dimension);
    // This is S1x... xSi
    IntVecT inner_array_sizes;
    for (auto i = 0; i < dimension; ++i) {
      auto s = reduce_mul(array_sizes.cbegin(), array_sizes.cbegin() + i, 1);
      inner_array_sizes.push_back(s);
    }

    int64_t start = 0;
    ParamVecT params;
    for (auto i = 0; i < dimension; ++i) {
      start += starts[i] * inner_array_sizes[i];
      auto stride = inner_array_sizes[i];
      auto trip = trips[i];
      params.emplace_back(stride, trip);
    }
    return AffinePattern(start, params);
  }

  static std::vector<AffinePattern>
  break_continuous_range_into_canonical_sub_regions(const IntVecT &array_sizes,
                                                    int64_t start,
                                                    int64_t trip) {
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

  static std::vector<AffinePattern>
  recursive_break_continuous_range_into_canonical_sub_regions(
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
      sub_regions.push_back(
          construct_canonical_sub_region(array_sizes, starts, trips));
    } else {
      if (p != 0) {
        // One sub region [P, B)
        IntVecT starts(ps);
        IntVecT trips(array_sizes);
        for (auto i = dim; i < dimension; ++i) {
          trips[i] = 1;
        }
        trips[dim] = t - p;
        sub_regions.push_back(
            construct_canonical_sub_region(array_sizes, starts, trips));
      }

      if (q != 0) {
        IntVecT starts(qs);
        starts[dim] = 0;
        IntVecT trips(array_sizes);
        for (auto i = dim; i < dimension; ++i) {
          trips[i] = 1;
        }
        trips[dim] = q;
        sub_regions.push_back(
            construct_canonical_sub_region(array_sizes, starts, trips));
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
          auto ret =
              recursive_break_continuous_range_into_canonical_sub_regions(
                  array_sizes, bs, cs, dim + 1);
          sub_regions.insert(sub_regions.end(), ret.begin(), ret.end());
        }
      }
    }
    return sub_regions;
  }

  static AffinePattern intersectSubRegions(const IntVecT &array_sizes,
                                           const AffinePattern &region1,
                                           const AffinePattern &region2);

  IntVecT generate_all_values() const {
    IntVecT values(get_total_trip(), 0);
    for (auto i = 0; i < get_total_trip(); ++i) {
      values[i] = operator()(i);
    }
    return values;
  }

  AffinePattern splitFromDim(int64_t dim);
};

std::ostream &operator<<(std::ostream &os, const AffinePattern &pattern);
std::ostream &operator<<(std::ostream &os,
                         const AffinePattern::IntVecT &intVec);

using AffinePatternVecT = std::vector<AffinePattern>;

#endif