#include "DataMoveCompiler.hh"

#include "base/trace.hh"
#include "debug/StreamPUM.hh"

PUMCommandVecT
DataMoveCompiler::compileStreamPair(const AffinePattern &srcStream,
                                    const AffinePattern &dstStream) const {
  canCompileStreamPair(srcStream, dstStream);

  // Generate the start alignments.
  auto srcStarts = getSubRegionStart(srcStream);
  auto dstStarts = getSubRegionStart(dstStream);

  std::vector<std::pair<int64_t, int64_t>> aligns;
  for (auto i = 0; i < dimension; ++i) {
    if (srcStarts[i] != dstStarts[i]) {
      aligns.emplace_back(i, dstStarts[i] - srcStarts[i]);
    }
  }
  assert(aligns.size() <= 1);

  // Record the reused dimension and reused count.
  std::vector<std::pair<int64_t, int64_t>> reuses;
  for (auto i = 0; i < dimension; ++i) {
    const auto &p = srcStream.params[i];
    if (p.stride == 0) {
      // This is reuse dimension.
      reuses.emplace_back(i, p.trip);
    }
  }
  assert(reuses.size() <= 1);

  if (!reuses.empty() && !aligns.empty()) {
    assert(reuses.size() == aligns.size());
    for (auto i = 0; i < reuses.size(); ++i) {
      // Assert that reuse and base align are along the same dimension
      assert(reuses[i].first == aligns[i].first);
    }
  }

  if (aligns.size() == 0) {
    // Nothing to align.
    assert(reuses.empty() && "Reuse when no align is not supported.");
    return PUMCommandVecT();
  }

  assert(reuses.empty() && "Reuse not supported yet.");

  /**
   * The overall compile flow:
   * 1. Compile for general data move insts to align along certain dimension.
   * 2. Mask commands the src sub-region without reuse.
   * 3. Mask commands with reuses (at the dst).
   * 4. Map commands to LLC banks.
   */

  auto commands = compileAligns(aligns);

  auto no_reuse_src_sub_region = removeReuseInSubRegion(srcStream);

  commands = maskCmdsBySubRegion(commands, no_reuse_src_sub_region);

  // Map commands to LLC configuration.
  commands = mapCmdsToLLC(commands);

  return commands;
}

AffinePattern
DataMoveCompiler::removeReuseInSubRegion(const AffinePattern &pattern) const {
  assert(is_canonical_sub_region(pattern, true));
  ParamVecT fixed_params;
  for (const auto &p : pattern.params) {
    auto stride = (p.stride == 0) ? 1 : p.stride;
    auto trip = (p.stride == 0) ? 1 : p.trip;
    fixed_params.emplace_back(stride, trip);
  }
  return AffinePattern(pattern.start, fixed_params);
}

PUMCommandVecT DataMoveCompiler::compileAligns(
    const std::vector<std::pair<int64_t, int64_t>> &aligns) const {
  assert(aligns.size() == 1);
  auto dim = aligns[0].first;
  auto distance = aligns[0].second;
  return compileAlign(dim, distance);
}

PUMCommandVecT DataMoveCompiler::compileAlign(int64_t dim,
                                              int64_t distance) const {
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
    return compileAlignLessThanTileSize(dim, distance);
  } else {
    assert(abs_dist % tile_sizes[dim] == 0);
    assert(false);
  }
}

PUMCommandVecT
DataMoveCompiler::compileAlignLessThanTileSize(int64_t dim,
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
  // Generate all active bitline-mask according to tile_sizes.
  commands.back().bitline_mask = AffinePattern::construct_canonical_sub_region(
      tile_sizes, AffinePattern::IntVecT(tile_sizes.size(), 0), tile_sizes);

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

/******************************************************************************
 * Mask commands by SubRegion.
 ******************************************************************************/

AffinePattern DataMoveCompiler::mergeMasks(const MaskVecT &masks,
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

AffinePattern
DataMoveCompiler::mergeBitlineMasks(const MaskVecT &bitlineMasks) const {
  assert(bitlineMasks.size() == dimension);
  IntVecT innerTileSizes;
  for (auto i = 0; i < dimension; ++i) {
    auto s = AffinePattern::reduce_mul(tile_sizes.cbegin(),
                                       tile_sizes.cbegin() + i, 1);
    innerTileSizes.push_back(s);
    auto trip = std::get<2>(bitlineMasks[i]);
    if (trip > tile_sizes[i]) {
      panic("BitlineMask Trip %ld Overflow Tile %ld.", trip, tile_sizes[i]);
    }
  }
  return mergeMasks(bitlineMasks, innerTileSizes);
}

AffinePattern
DataMoveCompiler::mergeTileMasks(const MaskVecT &tile_masks) const {
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
  return mergeMasks(tile_masks, inner_tile_nums);
}

AffinePattern DataMoveCompiler::intersectBitlineMasks(
    const AffinePattern &bitlineMask1,
    const AffinePattern &bitlineMask2) const {
  return AffinePattern::intersectSubRegions(tile_sizes, bitlineMask1,
                                            bitlineMask2);
}

void DataMoveCompiler::recursiveMaskSubRegionAtDim(
    const AffinePattern &subRegion, int64_t dim, MaskVecT &revBitlineMasks,
    MaskVecT &revTileMasks, AffinePatternVecT &finalBitlineMaskes,
    AffinePatternVecT &finalTileMasks) const {
  /**
    This implements the mask algorithm, and accumulate mask pattern
    along each dimension. At the end, we construct the overall mask pattern
    by merging bitline_masks and tile_masks.

    An key optimization is to check the merged bitline mask against
    inter-array commands' source bitline mask. If they have no intersection,
    we can ignore the command.

    This is done by leveraging the fact that both bitline masks are canonical
    sub-region within that tile, and take their interection.
   */
  if (dim == -1) {
    /**
     * ! Don't forget to reverse the masks first.
     */
    MaskVecT bitlineMasks = revBitlineMasks;
    std::reverse(bitlineMasks.begin(), bitlineMasks.end());
    MaskVecT tileMasks = revTileMasks;
    std::reverse(tileMasks.begin(), tileMasks.end());
    auto merged_bitline_masks = mergeBitlineMasks(bitlineMasks);
    auto merged_tile_masks = mergeTileMasks(tileMasks);
    finalBitlineMaskes.push_back(merged_bitline_masks);
    finalTileMasks.push_back(merged_tile_masks);
    return;
  }

  auto ps = getSubRegionStart(subRegion);
  auto qs = subRegion.get_trips();
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
      revBitlineMasks.emplace_back(tile_p, 1, t - tile_p);
      revTileMasks.emplace_back(a, 1, 1);
      if (t - tile_p > t) {
        panic("BitlineMask Trip %ld Overflow Tile %ld.", t - tile_p, t);
      }
      recursiveMaskSubRegionAtDim(subRegion, dim - 1, revBitlineMasks,
                                  revTileMasks, finalBitlineMaskes,
                                  finalTileMasks);
      revBitlineMasks.pop_back();
      revTileMasks.pop_back();
    }
    // [CxTi, P+Q)
    if (c < d) {
      revBitlineMasks.emplace_back(0, 1, tile_pq);
      revTileMasks.emplace_back(c, 1, 1);
      if (tile_pq > t) {
        panic("BitlineMask Trip %ld Overflow Tile %ld.", tile_pq, t);
      }
      recursiveMaskSubRegionAtDim(subRegion, dim - 1, revBitlineMasks,
                                  revTileMasks, finalBitlineMaskes,
                                  finalTileMasks);
      revBitlineMasks.pop_back();
      revTileMasks.pop_back();
    }
    if (b < c) {
      // [BxTi, CxTi)
      revBitlineMasks.emplace_back(0, 1, t);
      revTileMasks.emplace_back(b, 1, c - b);
      recursiveMaskSubRegionAtDim(subRegion, dim - 1, revBitlineMasks,
                                  revTileMasks, finalBitlineMaskes,
                                  finalTileMasks);
      revBitlineMasks.pop_back();
      revTileMasks.pop_back();
    }
  } else {
    // [P, P+Q)
    revBitlineMasks.emplace_back(tile_p, 1, tile_pq - tile_p);
    revTileMasks.emplace_back(a, 1, 1);
    if (tile_pq > t) {
      panic("BitlineMask %ld Overflow Tile %ld.", tile_pq, t);
    }
    recursiveMaskSubRegionAtDim(subRegion, dim - 1, revBitlineMasks,
                                revTileMasks, finalBitlineMaskes,
                                finalTileMasks);
    revBitlineMasks.pop_back();
    revTileMasks.pop_back();
  }
}

PUMCommandVecT
DataMoveCompiler::maskCmdsBySubRegion(const PUMCommandVecT &commands,
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
  AffinePatternVecT final_bitline_masks;
  AffinePatternVecT final_tile_masks;
  recursiveMaskSubRegionAtDim(sub_region, dimension - 1, bitline_masks,
                              tile_masks, final_bitline_masks,
                              final_tile_masks);

  for (const auto &command : commands) {
    for (int i = 0; i < final_bitline_masks.size(); ++i) {
      const auto &bitline_mask = final_bitline_masks.at(i);
      const auto &tile_mask = final_tile_masks.at(i);
      auto c = command;
      auto intersect = intersectBitlineMasks(c.bitline_mask, bitline_mask);
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

PUMCommandVecT
DataMoveCompiler::mapCmdsToLLC(const PUMCommandVecT &commands) const {
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
  auto tilePerLLCBank = llc_config.get_array_per_bank();
  auto numLLCBanks = llc_config.get_total_banks();

  IntVecT tile_nums;
  for (auto i = 0; i < dimension; ++i) {
    auto a = array_sizes[i];
    auto t = tile_sizes[i];
    auto s = (a + t - 1) / t;
    tile_nums.push_back(s);
  }

  // Construct the sub-region for each LLC bank.
  std::vector<AffinePatternVecT> llcBankSubRegions;
  for (auto i = 0; i < numLLCBanks; ++i) {
    llcBankSubRegions.push_back(
        AffinePattern::break_continuous_range_into_canonical_sub_regions(
            tile_nums, i * tilePerLLCBank, tilePerLLCBank));
  }

  // Process all commands.
  PUMCommandVecT ret;
  for (const auto &command : commands) {
    auto c(command);
    mapCmdToLLC(c, llcBankSubRegions);
    ret.push_back(c);
  }
  return ret;
}

void DataMoveCompiler::mapCmdToLLC(
    PUMCommand &command,
    const std::vector<AffinePatternVecT> &llcBankSubRegions) const {

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
    for (const auto &llc_sub_region : llcBankSubRegions[i]) {
      auto intersect = AffinePattern::intersectSubRegions(
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
    splitInterArrayCmdToLLC(command);
  }
}

void DataMoveCompiler::splitInterArrayCmdToLLC(PUMCommand &command) const {
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
