#ifndef __CPU_GEM_FORGE_DATA_MOVE_COMPILER_HH__
#define __CPU_GEM_FORGE_DATA_MOVE_COMPILER_HH__

#include "PUMCommand.hh"
#include "PUMHWConfiguration.hh"

class DataMoveCompiler {

  /**
    Take in a canonical tiling pattern and the LLC SRAM configuration,
    generate the data move instructions for certain aligning requirement.
    Key assumptions:
    1. Each tile is placed across one SRAM array's bitlines.
    2. Align requiremnts are specified through a combination of movement in each
    dimension.

    Given one source stream and one destination stream, we analyze the reuse
    and align requirements between them.

    Define CanonicalSubRegionPattern to be a pattern that iterates through
    a rectangular sub-region of the N-dimension array. It must be of pattern:

    P1 + P2xS1 + P3xS2xS1 + ... PnxS_{n-1}x...xS1
        : 1              : Q1
        : S1             : Q2
        : S1xS2          : Q3
        : ...
        : S1x...xS_{n-1} : Qn
    Pi >= 0, Qi > 0, Pi + Qi <= Si for i in [1, n]

    This defines a non-tiling region [P1, P1+Q1)x...x[Pn, Pn+Qn), and we
    immediately see that there is no reuse within this pattern.

    So far we assume the destination stream must be a CanonicalSubRegionPattern,
    while the source stream may reuse some dimension (0 stride):

    P1 + P2xS1 + P3xS2xS1 + ... PnxS_{n-1}x...xS1
        : 1              : Q1
        : 0              : Q2  // Reuse at this dimension.
        : 0              : Q3  // Another reuse.
        : ...
        : S1x...xS_{n-1} : Qn

    Also, the source and destination stream may have different start point, but
    the trip parameters across all dimension must match.

    For source stream with reuse, we replace the reused dimension with
    (stride=1, trip=1), which turns it back to a CanonicalSubRegionPattern.

    Then we analyze the difference between their start point to get the base
    align requirement, which is then multicasted according to the reuse
    dimension.

    Finally:
        The align requirement with multicast is used to generate the general
        commands applies to all SRAM arrays.
        The source CanonicalSubRegionPattern is used to mask the general
        commands. The LLC configuration is used to split the general commands
        according to the hardware topology and network.

    TODO: So far we assume no mixed dimension.
   */

public:
  using IntVecT = AffinePattern::IntVecT;
  using ParamVecT = AffinePattern::ParamVecT;

  PUMHWConfiguration llc_config;
  AffinePattern tile_pattern;
  int64_t dimension;
  IntVecT tile_sizes;
  IntVecT array_sizes;

  DataMoveCompiler(const PUMHWConfiguration &_llc_config,
                   const AffinePattern &_tile_pattern)
      : llc_config(_llc_config), tile_pattern(_tile_pattern) {

    assert(tile_pattern.is_canonical_tile());
    dimension = tile_pattern.params.size() / 2;
    auto ret = tile_pattern.get_canonical_tile_and_array_sizes();
    tile_sizes = std::move(ret.first);
    array_sizes = std::move(ret.second);
  }

  PUMCommandVecT compile(const AffinePattern &src_stream,
                         const AffinePattern &dst_stream) const {
    return analyze_stream_pair(src_stream, dst_stream);
  }

  IntVecT get_sub_region_start(const AffinePattern &sub_region) const {
    // This is S1x...xSi
    return sub_region.get_sub_region_start_to_array_size(array_sizes);
  }

  bool is_canonical_sub_region(const AffinePattern &pattern,
                               bool allow_reuse = false) const {
    return pattern.is_canonical_sub_region_to_array_size(array_sizes,
                                                         allow_reuse);
  }

  AffinePattern
  fix_reuse_in_canonical_sub_region(const AffinePattern &pattern) const {
    assert(is_canonical_sub_region(pattern, true));
    ParamVecT fixed_params;
    for (const auto &p : pattern.params) {
      auto stride = (p.stride == 0) ? 1 : p.stride;
      auto trip = (p.stride == 0) ? 1 : p.trip;
      fixed_params.emplace_back(stride, trip);
    }
    return AffinePattern(pattern.start, fixed_params);
  }

  void can_handle_stream_pair(const AffinePattern &src_stream,
                              const AffinePattern &dst_stream) const {
    assert(is_canonical_sub_region(dst_stream));
    assert(is_canonical_sub_region(src_stream, true));
    assert(src_stream.params.size() == dst_stream.params.size());
    auto src_trips = src_stream.get_trips();
    auto dst_trips = dst_stream.get_trips();
    for (auto i = 0; i < src_trips.size(); ++i) {
      assert(src_trips[i] == dst_trips[i]);
    }
  }

  PUMCommandVecT analyze_stream_pair(const AffinePattern &src_stream,
                                     const AffinePattern &dst_stream) const;

  PUMCommandVecT compile_align_and_reuse(
      const std::vector<std::pair<int64_t, int64_t>> &base_aligns,
      const std::vector<std::pair<int64_t, int64_t>> &reuses) const {
    assert(reuses.empty());
    assert(base_aligns.size() == 1);
    auto dim = base_aligns[0].first;
    auto distance = base_aligns[0].second;
    return compile_one_align(dim, distance);
  }

  PUMCommandVecT compile_one_align(int64_t dim, int64_t distance) const {
    /**
      Generate hierarchical data move commands:
      1. Within each tile (SRAM array).
      2. Boundary case:
          For dimensions between [0, dim), data is continous layout.
          For dimensions between (dim, D), data is incontinuous.
      Thus, we need one command to move [0, dim) to the correct location.
      Finally, we need to split traffic across tiles with different level
      of LLC configuration.
     */
    assert(dim < dimension);
    auto abs_dist = std::abs(distance);
    if (abs_dist < tile_sizes[dim]) {
      return handle_align_smaller_than_tile_size(dim, distance);
    } else {
      assert(abs_dist % tile_sizes[dim] == 0);
      assert(false);
    }
  }

  PUMCommandVecT handle_align_smaller_than_tile_size(int64_t dim,
                                                     int64_t distance) const;

  using MaskT = std::tuple<int64_t, int64_t, int64_t>;
  using MaskVecT = std::vector<MaskT>;

  AffinePattern merge_masks(const MaskVecT &masks,
                            const IntVecT &inner_sizes) const {
    int64_t start = 0;
    ParamVecT params;
    for (auto i = 0; i < dimension; ++i) {
      auto dim_start = std::get<0>(masks[i]);
      auto dim_stride = std::get<1>(masks[i]);
      auto dim_trip = std::get<2>(masks[i]);
      start += dim_start * inner_sizes[i];
      auto stride = dim_stride * inner_sizes[i];
      auto trip = dim_trip;
      params.emplace_back(stride, trip);
    }
    return AffinePattern(start, params);
  }

  AffinePattern merge_bitline_masks(const MaskVecT &bitline_masks) const {
    assert(bitline_masks.size() == dimension);
    IntVecT inner_tile_sizes;
    for (auto i = 0; i < dimension; ++i) {
      auto s = AffinePattern::reduce_mul(tile_sizes.cbegin(),
                                         tile_sizes.cbegin() + i, 1);
      inner_tile_sizes.push_back(s);
    }
    return merge_masks(bitline_masks, inner_tile_sizes);
  }

  AffinePattern merge_tile_masks(const MaskVecT &tile_masks) const {
    assert(tile_masks.size() == dimension);
    IntVecT tile_nums;
    for (auto i = 0; i < dimension; ++i) {
      auto a = array_sizes[i];
      auto t = tile_sizes[i];
      auto s = (a + t - 1) / t;
      tile_nums.push_back(s);
    }
    IntVecT inner_tile_nums;
    for (auto i = 0; i < dimension; ++i) {
      auto s = AffinePattern::reduce_mul(tile_nums.cbegin(),
                                         tile_nums.cbegin() + i, 1);
      inner_tile_nums.push_back(s);
    }
    return merge_masks(tile_masks, inner_tile_nums);
  }

  AffinePattern
  intersect_bitline_masks(const AffinePattern &bitline_mask1,
                          const AffinePattern &bitline_mask2) const {
    return AffinePattern::intersect_sub_regions(tile_sizes, bitline_mask1,
                                                bitline_mask2);
  }

  PUMCommandVecT
  mask_commands_by_sub_region(const PUMCommandVecT &commands,
                              const AffinePattern &sub_region) const {

    /**
      Recursively mask commands by sub_region dimensions.
      Starting from the outer-most dimension, let the current dimension be i.

      The sub region requires [Pi, Pi+Qi).
      Let the tile size be Ti, array size Si.

      Define:
      Ai = Pi // Ti
      Bi = (Pi + Ti - 1) // Ti
      Ci = (Pi + Qi) // Ti
      Di = (Pi + Qi + Ti - 1) // Ti

      In terms of an axis:

      |-------|-------|-------|-------|-------|-------|-------|
      A   P   B               C  P+Q  D

      If B < C:
      Mask [P,    BxTi)
      Mask [CxTi, P+Q)
      No Mask [BxTi, CxTi)
      If B = C:
      Mask [P,    BxTi)
      Mask [CxTi, P+Q)
      If B < C:
      Mask [P, P+Q)

      And then go to the next dimension.
     */
    PUMCommandVecT masked_commands;
    MaskVecT bitline_masks;
    MaskVecT tile_masks;
    recursive_mask_commands_at_dim(commands, sub_region, dimension - 1,
                                   bitline_masks, tile_masks, masked_commands);
    return masked_commands;
  }

  void recursive_mask_commands_at_dim(const PUMCommandVecT &commands,
                                      const AffinePattern &sub_region,
                                      int64_t dim, MaskVecT &bitline_maskes,
                                      MaskVecT &tile_masks,
                                      PUMCommandVecT &masked_commands) const {
    /**
      This implements the above mask algorithm, and accumulate mask pattern
      along each dimension. At the end, we construct the overall mask pattern
      by merging bitline_masks and tile_masks.

      An key optimization is to check the merged bitline mask against
      inter-array commands' source bitline mask. If they have no intersection,
      we can ignore the command.

      This is done by leveraging the fact that both bitline masks are canonical
      sub-region within that tile, and take their interection.
     */
    if (dim == -1) {
      auto merged_bitline_masks = merge_bitline_masks(bitline_maskes);
      auto merged_tile_masks = merge_tile_masks(tile_masks);
      for (const auto &command : commands) {
        auto c = command;
        c.bitline_mask = merged_bitline_masks;
        c.tile_mask = merged_tile_masks;
        if (c.type == "inter-array") {
          auto intersect =
              intersect_bitline_masks(c.src_bitline_mask, merged_bitline_masks);
          if (intersect.get_total_trip() == 0) {
            // Empty intersection.
            continue;
          }
          c.bitline_mask = intersect;
        }
        masked_commands.push_back(c);
      }
      return;
    }

    auto ps = get_sub_region_start(sub_region);
    auto qs = sub_region.get_trips();
    auto p = ps[dim];
    auto q = qs[dim];
    auto t = tile_sizes[dim];
    auto a = p / t;
    auto b = (p + t - 1) / t;
    auto c = (p + q) / t;
    auto d = (p + q + t - 1) / t;

    auto tile_p = p - a * t;
    auto tile_pq = p + q - c * t;
    if (b <= c) {
      // [P, BxTi)
      if (a < b) {
        bitline_maskes.emplace_back(tile_p, 1, t - tile_p);
        tile_masks.emplace_back(a, 1, 1);
        recursive_mask_commands_at_dim(commands, sub_region, dim - 1,
                                       bitline_maskes, tile_masks,
                                       masked_commands);
        bitline_maskes.pop_back();
        tile_masks.pop_back();
      }
      // [CxTi, P+Q)
      if (c < d) {
        bitline_maskes.emplace_back(0, 1, tile_pq);
        tile_masks.emplace_back(c, 1, 1);
        recursive_mask_commands_at_dim(commands, sub_region, dim - 1,
                                       bitline_maskes, tile_masks,
                                       masked_commands);
        bitline_maskes.pop_back();
        tile_masks.pop_back();
      }
      if (b < c) {
        // [BxTi, CxTi)
        bitline_maskes.emplace_back(0, 1, t);
        tile_masks.emplace_back(b, 1, c - b);
        recursive_mask_commands_at_dim(commands, sub_region, dim - 1,
                                       bitline_maskes, tile_masks,
                                       masked_commands);
        bitline_maskes.pop_back();
        tile_masks.pop_back();
      }
    } else {
      // [P, P+Q)
      bitline_maskes.emplace_back(tile_p, 1, tile_pq);
      tile_masks.emplace_back(a, 1, 1);
      recursive_mask_commands_at_dim(commands, sub_region, dim - 1,
                                     bitline_maskes, tile_masks,
                                     masked_commands);
      bitline_maskes.pop_back();
      tile_masks.pop_back();
    }
  }

  PUMCommandVecT map_commands_to_llc(const PUMCommandVecT &commands) const {
    /**
      Here we map commands to LLC.

      Since the number of tiles in each dimension may not be a divisor of
      the LLC SRAM arrays configuration, it is very challenging to derive
      an analytical close form to commands in all LLC slice. Therefore,
      here we explicitly generate the mask for each LLC slice.

      We do this in the following steps.
      1. Each LLC slice will have a number of SRAM arrays, and tiles are
      mapped continuously to these slices, with one tile per SRAM array.
      2. We first split the SRAM arrays into canonical sub-regions in the
      tile coordinate, then for each sub-regions we take the intersection
      with the command's tile mask to generate the specific tile mask
      within that LLC slice.
      3. With in each slice, we then split commands according to the tree
      structure.
     */
    auto tile_per_llc_bank = llc_config.get_array_per_bank();
    auto total_llc_banks = llc_config.get_total_banks();

    IntVecT tile_nums;
    for (auto i = 0; i < dimension; ++i) {
      auto a = array_sizes[i];
      auto t = tile_sizes[i];
      auto s = (a + t - 1) / t;
      tile_nums.push_back(s);
    }

    // Construct the sub-region for each LLC bank.
    std::vector<AffinePatternVecT> llc_bank_sub_regions;
    for (auto i = 0; i < total_llc_banks; ++i) {
      llc_bank_sub_regions.push_back(
          AffinePattern::break_continuous_range_into_canonical_sub_regions(
              tile_nums, i * tile_per_llc_bank, tile_per_llc_bank));
    }

    // Process all commands.
    PUMCommandVecT ret;
    for (const auto &command : commands) {
      auto c(command);
      map_command_to_llc(c, llc_bank_sub_regions);
      ret.push_back(c);
    }
    return ret;
  }

  void map_command_to_llc(
      PUMCommand &command,
      const std::vector<AffinePatternVecT> &llc_bank_sub_regions) const {

    auto total_llc_banks = llc_config.get_total_banks();

    IntVecT tile_nums;
    for (auto i = 0; i < dimension; ++i) {
      auto a = array_sizes[i];
      auto t = tile_sizes[i];
      auto s = (a + t - 1) / t;
      tile_nums.push_back(s);
    }

    for (auto i = 0; i < total_llc_banks; ++i) {
      AffinePatternVecT llc_tiles;
      for (const auto &llc_sub_region : llc_bank_sub_regions[i]) {
        auto intersect = AffinePattern::intersect_sub_regions(
            tile_nums, command.tile_mask, llc_sub_region);
        if (intersect.get_total_trip() == 0) {
          // Empty intersection.
          continue;
        }
        llc_tiles.push_back(intersect);
      }
      command.llc_commands.push_back(llc_tiles);
    }

    if (command.type == "inter-array") {
      split_inter_array_command_to_llc(command);
    }
  }

  void split_inter_array_command_to_llc(PUMCommand &command) const {
    /**
      First start to scan each level of the tree in the way.
      Then handle inter-ways.
      Finally across LLC banks.

      At each level, we model it as this:

      [  S  ] [  S  ] [  S  ] ... [  S  ] [  S  ] [  S  ]
         |       |       |           |       |       |
          -------------------------------------------
                              DxS

      Each sub-tree has S arrays, with D sub-trees.

      If abs_tile_dist >= DxS: Nothing to move within this level.

      Otherwise, define
          M = abs_tile_dist // S
          N = abs_tile_dist % S.

      If tile_dist > 0
        If abs_tile_dist >= S
            The first part:
                0 : 1 : S-N : S : D-M
            The second part:
                S-N : 1 : N : S : D-M-1
        If abs_tile_dist < S
            Only part:
                S-N : 1 : N : S : D-1

        This can be merged into:
        The first part only comes when abs_tile_dist >= S
                0 : 1 : S-N : S : D-M
        The second part is always the same:
                S-N : 1 : N : S : D-M-1

      If tile_dist < 0
        If abs_tile_dist >= S
            The first part:
                (M+1)*S : 1 : N : S : D-M-1
            The second part:
                M*S+N : 1 : S-N : S : D-M
        If abs_tile_dist < S
            Only part:
                S : 1 : N : S : D-1

        This can be merged into:
        The first part is always the same:
                (M+1)*S : 1 : N : S : D-M-1
        The second part only comes when abs_tile_dist >=S
                M*S+N : 1 : S-N : S : D-M
     */

    auto tile_dist = command.tile_dist;

    auto split = [](int64_t s, int64_t d,
                    int64_t tile_dist) -> AffinePatternVecT {
      AffinePatternVecT splits;
      auto abs_tile_dist = std::abs(tile_dist);
      if (abs_tile_dist < s * d) {
        auto m = abs_tile_dist / s;
        auto n = abs_tile_dist % s;
        if (tile_dist > 0) {
          // First part.
          if (abs_tile_dist >= s) {
            ParamVecT params;
            params.push_back(AffinePattern::Param(1, s - n));
            params.push_back(AffinePattern::Param(s, d - m));
            AffinePattern pattern(0, params);
            splits.push_back(pattern);
          }
          // Second part.
          ParamVecT params;
          params.push_back(AffinePattern::Param(1, n));
          params.push_back(AffinePattern::Param(s, d - m - 1));
          AffinePattern pattern(s - n, params);
          splits.push_back(pattern);
        } else {
          // First part.
          ParamVecT params;
          params.push_back(AffinePattern::Param(1, n));
          params.push_back(AffinePattern::Param(s, d - m - 1));
          AffinePattern pattern((m + 1) * s, params);
          splits.push_back(pattern);
          // Second part.
          if (abs_tile_dist >= s) {
            ParamVecT params;
            params.push_back(AffinePattern::Param(1, s - n));
            params.push_back(AffinePattern::Param(s, d - m));
            AffinePattern pattern(m * s + n, params);
            splits.push_back(pattern);
          }
        }
      }
      return splits;
    };

    auto &inter_array_splits = command.inter_array_splits;

    auto prev_level_tiles = 1;
    auto cur_level_tiles = llc_config.tree_degree;
    while (cur_level_tiles <= llc_config.array_per_way) {
      auto s = prev_level_tiles;
      auto d = llc_config.tree_degree;
      auto splits = split(s, d, tile_dist);
      inter_array_splits.push_back(splits);

      prev_level_tiles = cur_level_tiles;
      cur_level_tiles *= llc_config.tree_degree;
    }

    // Inter LLC ways.
    {
      auto splits =
          split(llc_config.array_per_way, llc_config.way_per_bank, tile_dist);
      inter_array_splits.push_back(splits);
    }

    // Inter LLC banks.
    {
      auto splits = split(llc_config.get_array_per_bank(),
                          llc_config.get_total_banks(), tile_dist);
      inter_array_splits.push_back(splits);
    }
  }
};

#endif