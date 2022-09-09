#ifndef __CPU_GEM_FORGE_AFFINE_PATTERN_HH__
#define __CPU_GEM_FORGE_AFFINE_PATTERN_HH__

#include "AffinePatternImpl.hh"
#include "MaxVector.hh"

#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <tuple>
#include <vector>

#include "TDFG.pb.h"
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse tensor dataflow graph."
#endif

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
    Param(int64_t _stride, int64_t _trip) : stride(_stride), trip(_trip) {}
  };

  static constexpr size_t MaxDimension = 8;

  // using ParamVecT = std::vector<Param>;
  using ParamVecT = MaxVector<Param, MaxDimension>;
  using IntVecT = std::vector<int64_t>;

  int64_t start;
  ParamVecT params;

  // Default create an empty pattern.
  AffinePattern() : start(0) { params.emplace_back(1, 0); }
  AffinePattern(int64_t _start, ParamVecT _params)
      : start(_start), params(std::move(_params)) {}
  AffinePattern(::LLVM::TDG::AffinePattern tdgAffinePattern);

  ::LLVM::TDG::AffinePattern toTDGAffinePattern() const;

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

  int64_t numDimension() const { return this->params.size(); }

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

  IntVecT getArraySize() const {
    auto dimension = params.size() / 2;
    IntVecT array_sizes;
    for (auto i = 0; i < dimension; ++i) {
      array_sizes.push_back(params[i].trip * params[i + dimension].trip);
    }
    return array_sizes;
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
    auto dimension = tile_sizes.size();
    assert(tile_sizes.size() == array_sizes.size());
    for (auto i = 0; i < dimension; ++i) {
      assert(array_sizes[i] % tile_sizes[i] == 0);
      assert(array_sizes[i] >= tile_sizes[i]);
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

  IntVecT getTrips() const {
    IntVecT ret;
    for (const auto &p : params) {
      ret.push_back(p.trip);
    }
    return ret;
  }

  int64_t getTotalTrip() const {
    int64_t ret = 1;
    for (const auto &p : params) {
      ret *= p.trip;
    }
    return ret;
  }

  bool is_empty() const { return getTotalTrip() == 0; }

  bool check_is_reverse(const AffinePattern &reverse) const {
    for (int64_t i = 0; i < getTotalTrip(); ++i) {
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

  static IntVecT getArrayPosition(const IntVecT &arraySizes, int64_t linearPos);

  IntVecT getSubRegionStartToArraySize(const IntVecT &arraySizes) const {
    return getArrayPosition(arraySizes, start);
  }

  bool isSubRegionToArraySize(const IntVecT &array_sizes,
                              bool allow_reuse = false) const;

  static AffinePattern constructSubRegion(const IntVecT &arraySizes,
                                          const IntVecT &starts,
                                          const IntVecT &trips) {
    auto dimension = arraySizes.size();
    assert(starts.size() == dimension);
    assert(trips.size() == dimension);
    // This is S1x... xSi
    IntVecT innerArraySizes;
    for (auto i = 0; i < dimension; ++i) {
      auto s = reduce_mul(arraySizes.cbegin(), arraySizes.cbegin() + i, 1);
      innerArraySizes.push_back(s);
    }

    int64_t start = 0;
    ParamVecT params;
    for (auto i = 0; i < dimension; ++i) {
      start += starts[i] * innerArraySizes[i];
      auto stride = innerArraySizes[i];
      auto trip = trips[i];
      params.emplace_back(stride, trip);
    }
    return AffinePattern(start, params);
  }

  static std::vector<AffinePattern>
  break_continuous_range_into_canonical_sub_regions(const IntVecT &array_sizes,
                                                    int64_t start,
                                                    int64_t trip);

  static std::vector<AffinePattern>
  recursive_break_continuous_range_into_canonical_sub_regions(
      const IntVecT &array_sizes, IntVecT &ps, IntVecT &qs, int64_t dim);

  static AffinePattern removeReuseInSubRegion(const IntVecT &arraySizes,
                                              const AffinePattern &pattern);

  static AffinePattern intersectSubRegions(const IntVecT &array_sizes,
                                           const AffinePattern &region1,
                                           const AffinePattern &region2);

  static AffinePattern unionSubRegions(const IntVecT &array_sizes,
                                       const AffinePattern &region1,
                                       const AffinePattern &region2);

  IntVecT generate_all_values() const {
    IntVecT values(getTotalTrip(), 0);
    for (auto i = 0; i < getTotalTrip(); ++i) {
      values[i] = operator()(i);
    }
    return values;
  }

  AffinePattern splitFromDim(int64_t dim);
  void mergeOutDim(const AffinePattern &splitOutDim);
};

std::ostream &operator<<(std::ostream &os, const AffinePattern &pattern);
std::ostream &operator<<(std::ostream &os,
                         const AffinePattern::IntVecT &intVec);

using AffinePatternVecT = std::vector<AffinePattern>;

/**
 * Conversion between AffinePattern and AffinePatternImpl.
 */

template <size_t dimension, typename T>
AffinePatternImpl<dimension, T> getAffinePatternImpl(const AffinePattern &p) {
  assert(p.numDimension() == dimension);
  typename AffinePatternImpl<dimension, T>::ParamVecT params;
  for (auto i = 0; i < dimension; ++i) {
    params[i].trip = p.params[i].trip;
    params[i].stride = p.params[i].stride;
  }
  return AffinePatternImpl<dimension, T>(p.start, params);
}

template <size_t dimension, typename T>
AffinePattern
getAffinePatternFromImpl(const AffinePatternImpl<dimension, T> &p) {
  AffinePattern::ParamVecT params;
  for (auto i = 0; i < dimension; ++i) {
    params.emplace_back(p.params[i].stride, p.params[i].trip);
  }
  return AffinePattern(p.start, params);
}

#endif
