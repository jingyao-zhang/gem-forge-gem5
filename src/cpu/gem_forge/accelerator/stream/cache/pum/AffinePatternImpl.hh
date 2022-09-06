#ifndef __CPU_GEM_FORGE_AFFINE_PATTERN_IMPL_HH__
#define __CPU_GEM_FORGE_AFFINE_PATTERN_IMPL_HH__

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <tuple>
#include <vector>

#include "base/logging.hh"

/**
 * To optimize for the performance, we use template to specialize some common
 * affine pattern for certain dimensions.
 */

template <typename T> constexpr T constPow(T a, T b) {
  return b == 0 ? 1 : a * constPow(a, b - 1);
}

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

  /**
   * For a given dimension array, we always know the maximal number of canonical
   * sub-regions that a continuous range could be broken into. This is used as
   * an upper bound on when we can break the LLC banks into sub-regions.
   *
   * The formula is 3 ^ (dimension - 1).
   */
  static constexpr size_t MaxSubRegionsForContinuousRange =
      constPow(3ul, dimension);

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

  /**
   * Represents the result sub-regions of breaking a continuous range.
   */
  struct ContinuousRangeSubRegions {
    std::array<ThisT, MaxSubRegionsForContinuousRange> subRegions;
    int count = 0;
    void add(ThisT subRegion) {
      assert(count < MaxSubRegionsForContinuousRange);
      subRegions.at(count) = std::move(subRegion);
      count++;
    }
  };

  static ContinuousRangeSubRegions
  break_continuous_range_into_canonical_sub_regions(const IntVecT &array_sizes,
                                                    T start, T trip) {
    // Handle possible cases when start/trip overflow array_sizes.
    ContinuousRangeSubRegions ret;
    auto totalSize = reduce_mul(array_sizes.begin(), array_sizes.end(), 1);
    if (start >= totalSize || start + trip <= 0) {
      // No overlap at all.
      return ret;
    }
    trip = std::min(trip, totalSize - start);
    auto ps = getArrayPosition(array_sizes, start);
    auto qs = getArrayPosition(array_sizes, start + trip);
    RecursiveBreakContinuousRangeIntoCanonicalSubRegions<dimension, false>::run(
        array_sizes, ps, qs, ret);

    return ret;
  }

  template <size_t remain_dim, bool dummy>
  struct RecursiveBreakContinuousRangeIntoCanonicalSubRegions {
    static void run(const IntVecT &array_sizes, IntVecT &ps, IntVecT &qs,
                    ContinuousRangeSubRegions &ret) {

      /**
      This method breaks a continuous range [start, start + trip) into a list of
      sub regions. Specifically, it aligns the start and and to each dimension
      by creating new sub regions if the mod is not zero.

      |--------|-------|-------|-------|-------|-------|-------|--------|
               A   P   B       C   Q   D

      Create a sub region for [P, B), [C, Q) and keep [B, C) continuous.
      */

      static constexpr size_t dim = dimension - remain_dim;

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
        ret.add(constructSubRegion(array_sizes, starts, trips));
      } else {
        if (p != 0) {
          // One sub region [P, B)
          IntVecT starts(ps);
          IntVecT trips(array_sizes);
          for (auto i = dim; i < dimension; ++i) {
            trips[i] = 1;
          }
          trips[dim] = t - p;
          ret.add(constructSubRegion(array_sizes, starts, trips));
        }

        if (q != 0) {
          IntVecT starts(qs);
          starts[dim] = 0;
          IntVecT trips(array_sizes);
          for (auto i = dim; i < dimension; ++i) {
            trips[i] = 1;
          }
          trips[dim] = q;
          ret.add(constructSubRegion(array_sizes, starts, trips));
        }

        if (!high_dim_match) {
          // There is more to match.
          assert(remain_dim > 1);
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
            RecursiveBreakContinuousRangeIntoCanonicalSubRegions<
                remain_dim - 1, false>::run(array_sizes, bs, cs, ret);
          }
        }
      }
    }
  };

  /**
   * Partial specialization to break the chain.
   */
  template <bool dummy>
  struct RecursiveBreakContinuousRangeIntoCanonicalSubRegions<0, dummy> {
    static void run(const IntVecT &array_sizes, IntVecT &ps, IntVecT &qs,
                    ContinuousRangeSubRegions &ret) {
      panic("Recursive break continuous range into canonical sub-regions "
            "remain_dim = 0");
    }
  };
};

#endif