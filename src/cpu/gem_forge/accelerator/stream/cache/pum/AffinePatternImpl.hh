#ifndef __CPU_GEM_FORGE_AFFINE_PATTERN_IMPL_HH__
#define __CPU_GEM_FORGE_AFFINE_PATTERN_IMPL_HH__

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <tuple>
#include <vector>

/**
 * To optimize for the performance, we use template to specialize some common
 * affine pattern for certain dimensions.
 */

template <size_t dimension, typename T = int64_t> class AffinePatternImpl {
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
    T stride;
    T trip;
    Param(T _stride, T _trip) : stride(_stride), trip(_trip) {}
    Param() : stride(0), trip(0) {}
  };

  using ParamVecT = std::array<Param, dimension>;
  using IntVecT = std::array<T, dimension>;
  using HeapIntVecT = std::vector<T>;
  using ThisT = AffinePatternImpl<dimension, T>;

  T start;
  ParamVecT params;
  IntVecT trips;

  AffinePatternImpl(T _start, ParamVecT _params)
      : start(_start), params(std::move(_params)) {
    for (int i = 0; i < dimension; ++i) {
      trips[i] = params[i].trip;
    }
  }

  AffinePatternImpl() : start(0) {
    for (int i = 0; i < dimension; ++i) {
      params[i].trip = 0;
      trips[i] = 0;
    }
  }

  const IntVecT &getTrips() const { return trips; }

  T getTotalTrip() const {
    T ret = 1;
    for (const auto &p : params) {
      ret *= p.trip;
    }
    return ret;
  }

  static T reduce_mul(typename IntVecT::const_iterator s,
                      typename IntVecT::const_iterator t, T init) {
    auto ret = init;
    while (s != t) {
      ret *= *s;
      ++s;
    }
    return ret;
  }

  static IntVecT getFixSizedIntVec(const HeapIntVecT &v) {
    assert(v.size() == dimension);
    IntVecT ret;
    for (int i = 0; i < dimension; ++i) {
      ret[i] = v[i];
    }
    return ret;
  }

  static HeapIntVecT getHeapIntVec(const IntVecT &v) {
    HeapIntVecT ret;
    for (int i = 0; i < dimension; ++i) {
      ret.push_back(v[i]);
    }
    return ret;
  }

  static IntVecT getArrayPosition(const IntVecT &arraySizes, T linearPos) {
    /**
     * Given a linear position, return the position according to the array
     * dimension.
     */
    // This is S1x... xSi
    IntVecT inner_array_sizes;
    inner_array_sizes[0] = 1;
    for (auto i = 1; i < dimension; ++i) {
      inner_array_sizes[i] = inner_array_sizes[i - 1] * arraySizes[i - 1];
    }
    IntVecT pos;
    auto cur_pos = std::abs(linearPos);
    for (int i = dimension - 1; i >= 0; --i) {
      auto p = cur_pos / inner_array_sizes[i];

      pos[i] = (linearPos > 0) ? p : -p;

      cur_pos = cur_pos % inner_array_sizes[i];
    }
    return pos;
  }

  static ThisT intersectSubRegions(const IntVecT &array_sizes,
                                   const ThisT &region1, const ThisT &region2) {

    auto starts1 = getArrayPosition(array_sizes, region1.start);
    auto starts2 = getArrayPosition(array_sizes, region2.start);
    const auto &trips1 = region1.getTrips();
    const auto &trips2 = region2.getTrips();
    IntVecT intersect_starts;
    IntVecT intersect_trips;
    for (auto i = 0; i < dimension; ++i) {
      auto s1 = starts1[i];
      auto t1 = trips1[i];
      auto s2 = starts2[i];
      auto t2 = trips2[i];
      auto ss = std::max(s1, s2);
      auto tt = std::min(s1 + t1, s2 + t2) - ss;
      if (s1 >= s2 + t2 || s2 >= s1 + t1) {
        // None means empty intersection.
        // This will make the TotalTrip zero.
        tt = 0;
      }
      intersect_starts[i] = ss;
      intersect_trips[i] = tt;
    }
    return constructSubRegion(array_sizes, intersect_starts, intersect_trips);
  }

  static ThisT constructSubRegion(const IntVecT &arraySizes,
                                  const IntVecT &starts, const IntVecT &trips) {
    // This is S1x... xSi
    IntVecT innerArraySizes;
    innerArraySizes[0] = 1;
    for (auto i = 1; i < dimension; ++i) {
      innerArraySizes[i] = innerArraySizes[i - 1] * arraySizes[i - 1];
    }

    T start = 0;
    ParamVecT params;
    for (auto i = 0; i < dimension; ++i) {
      start += starts[i] * innerArraySizes[i];
      auto stride = innerArraySizes[i];
      auto trip = trips[i];
      params[i].trip = trip;
      params[i].stride = stride;
    }
    return ThisT(start, params);
  }
};

#endif