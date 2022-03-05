#include "DataMoveCompiler.hh"

#include "base/trace.hh"
#include "debug/StreamPUM.hh"

PUMCommandVecT
DataMoveCompiler::analyze_stream_pair(const AffinePattern &src_stream,
                                      const AffinePattern &dst_stream) const {
  can_handle_stream_pair(src_stream, dst_stream);

  auto src_starts = get_sub_region_start(src_stream);
  auto dst_starts = get_sub_region_start(dst_stream);

  std::vector<std::pair<int64_t, int64_t>> base_aligns;
  for (auto i = 0; i < dimension; ++i) {
    if (src_starts[i] != dst_starts[i]) {
      base_aligns.emplace_back(i, dst_starts[i] - src_starts[i]);
    }
  }
  assert(base_aligns.size() <= 1);

  std::vector<std::pair<int64_t, int64_t>> reuses;
  for (auto i = 0; i < dimension; ++i) {
    const auto &p = src_stream.params[i];
    if (p.stride == 0) {
      // This is reuse dimension.
      reuses.emplace_back(i, p.trip);
    }
  }
  assert(reuses.size() <= 1);

  if (!reuses.empty() && !base_aligns.empty()) {
    assert(reuses.size() == base_aligns.size());
    for (auto i = 0; i < reuses.size(); ++i) {
      // Assert that reuse and base align are along the same dimension
      assert(reuses[i].first == base_aligns[i].first);
    }
  }

  if (base_aligns.size() == 0) {
    // Nothing to align.
    return PUMCommandVecT();
  }

  auto commands = compile_align_and_reuse(base_aligns, reuses);

  auto no_reuse_src_sub_region = fix_reuse_in_canonical_sub_region(src_stream);

  commands = mask_commands_by_sub_region(commands, no_reuse_src_sub_region);

  // Map commands to LLC configuration.
  commands = map_commands_to_llc(commands);

  return commands;
}

PUMCommandVecT
DataMoveCompiler::handle_align_smaller_than_tile_size(int64_t dim,
                                                      int64_t distance) const {
  PUMCommandVecT commands;

  auto abs_dist = std::abs(distance);
  assert(abs_dist < tile_sizes[dim]);

  // Traffic within this tile.
  auto bitlines = distance;
  for (auto i = 0; i < dim; ++i) {
    bitlines *= tile_sizes[i];
  }
  commands.emplace_back();
  commands.back().type = "intra-array";
  commands.back().bitline_dist = bitlines;
  // Generate all active bitline-mask.
  auto totalTileSize =
      AffinePattern::reduce_mul(tile_sizes.begin(), tile_sizes.end(), 1);
  commands.back().bitline_mask =
      AffinePattern(0, ParamVecT(1, AffinePattern::Param(1, totalTileSize)));

  // Boundary case.
  auto move_tile_dist = 1;
  for (auto i = 0; i < dim; ++i) {
    move_tile_dist *= array_sizes[i] / tile_sizes[i];
  }
  if (distance < 0) {
    move_tile_dist *= -1;
  }

  // Construct the sub-region of front and back.
  IntVecT front_starts;
  IntVecT front_trips;
  for (auto i = 0; i < dim; ++i) {
    front_starts.push_back(0);
    front_trips.push_back(tile_sizes[i]);
  }
  front_starts.push_back(0);
  front_trips.push_back(abs_dist);
  for (auto i = dim + 1; i < dimension; ++i) {
    front_starts.push_back(0);
    front_trips.push_back(tile_sizes[i]);
  }

  IntVecT back_starts;
  IntVecT back_trips;
  for (auto i = 0; i < dim; ++i) {
    back_starts.push_back(0);
    back_trips.push_back(tile_sizes[i]);
  }
  back_starts.push_back(tile_sizes[dim] - abs_dist);
  back_trips.push_back(abs_dist);
  for (auto i = dim + 1; i < dimension; ++i) {
    back_starts.push_back(0);
    back_trips.push_back(tile_sizes[i]);
  }

  auto front_pattern = AffinePattern::construct_canonical_sub_region(
      tile_sizes, front_starts, front_trips);
  auto back_pattern = AffinePattern::construct_canonical_sub_region(
      tile_sizes, back_starts, back_trips);

  commands.emplace_back();
  commands.back().type = "inter-array";
  commands.back().tile_dist = move_tile_dist;
  if (distance > 0) {
    // Move forward.
    commands.back().bitline_mask = back_pattern;
    commands.back().dst_bitline_mask = front_pattern;
  } else {
    // Move backward.
    commands.back().bitline_mask = front_pattern;
    commands.back().dst_bitline_mask = back_pattern;
  }

  DPRINTF(StreamPUM, "Inter-Array command %s", commands.back());

  return commands;
}

PUMCommandVecT DataMoveCompiler::mask_commands_by_sub_region(
    const PUMCommandVecT &commands, const AffinePattern &sub_region) const {

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
  AffinePatternVecT final_bitline_masks;
  AffinePatternVecT final_tile_masks;
  recursive_mask_sub_region_at_dim(sub_region, dimension - 1, bitline_masks,
                                   tile_masks, final_bitline_masks,
                                   final_tile_masks);

  for (const auto &command : commands) {
    for (int i = 0; i < final_bitline_masks.size(); ++i) {
      const auto &bitline_mask = final_bitline_masks.at(i);
      const auto &tile_mask = final_tile_masks.at(i);
      auto c = command;
      auto intersect = intersect_bitline_masks(c.bitline_mask, bitline_mask);
      if (intersect.get_total_trip() == 0) {
        // Empty intersection.
        continue;
      }
      c.bitline_mask = intersect;
      c.tile_mask = tile_mask;
      masked_commands.push_back(c);
    }
  }

  return masked_commands;
}

void DataMoveCompiler::recursive_mask_sub_region_at_dim(
    const AffinePattern &sub_region, int64_t dim, MaskVecT &bitline_maskes,
    MaskVecT &tile_masks, AffinePatternVecT &final_bitline_maskes,
    AffinePatternVecT &final_tile_masks) const {
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
    final_bitline_maskes.push_back(merged_bitline_masks);
    final_tile_masks.push_back(merged_tile_masks);
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
      recursive_mask_sub_region_at_dim(sub_region, dim - 1, bitline_maskes,
                                       tile_masks, final_bitline_maskes,
                                       final_tile_masks);
      bitline_maskes.pop_back();
      tile_masks.pop_back();
    }
    // [CxTi, P+Q)
    if (c < d) {
      bitline_maskes.emplace_back(0, 1, tile_pq);
      tile_masks.emplace_back(c, 1, 1);
      recursive_mask_sub_region_at_dim(sub_region, dim - 1, bitline_maskes,
                                       tile_masks, final_bitline_maskes,
                                       final_tile_masks);
      bitline_maskes.pop_back();
      tile_masks.pop_back();
    }
    if (b < c) {
      // [BxTi, CxTi)
      bitline_maskes.emplace_back(0, 1, t);
      tile_masks.emplace_back(b, 1, c - b);
      recursive_mask_sub_region_at_dim(sub_region, dim - 1, bitline_maskes,
                                       tile_masks, final_bitline_maskes,
                                       final_tile_masks);
      bitline_maskes.pop_back();
      tile_masks.pop_back();
    }
  } else {
    // [P, P+Q)
    bitline_maskes.emplace_back(tile_p, 1, tile_pq);
    tile_masks.emplace_back(a, 1, 1);
    recursive_mask_sub_region_at_dim(sub_region, dim - 1, bitline_maskes,
                                     tile_masks, final_bitline_maskes,
                                     final_tile_masks);
    bitline_maskes.pop_back();
    tile_masks.pop_back();
  }
}

PUMCommandVecT
DataMoveCompiler::map_commands_to_llc(const PUMCommandVecT &commands) const {
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

void DataMoveCompiler::map_command_to_llc(
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

void DataMoveCompiler::split_inter_array_command_to_llc(
    PUMCommand &command) const {
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