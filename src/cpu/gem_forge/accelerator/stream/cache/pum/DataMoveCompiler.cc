#include "DataMoveCompiler.hh"

#include "base/trace.hh"
#include "debug/MLCStreamPUM.hh"

DataMoveCompiler::DataMoveCompiler(const PUMHWConfiguration &_llc_config,
                                   const AffinePattern &_tile_pattern)
    : llc_config(_llc_config), tile_pattern(_tile_pattern) {

  assert(tile_pattern.isCanonicalTile());
  dimension = tile_pattern.params.size() / 2;
  auto ret = tile_pattern.getTileAndArraySize();
  tile_sizes = std::move(ret.first);
  array_sizes = std::move(ret.second);

  for (auto i = 0; i < dimension; ++i) {
    auto a = array_sizes[i];
    auto t = tile_sizes[i];
    auto s = (a + t - 1) / t;
    tile_nums.push_back(s);
  }
}

bool DataMoveCompiler::canTurnStrideIntoMask(
    const AffinePattern &pattern) const {
  auto starts = pattern.getSubRegionStartToArraySize(this->array_sizes);

  auto curDimStride = 1;
  auto dims = pattern.params.size();
  if (dims > this->dimension) {
    DPRINTF(MLCStreamPUM, "[NoMask] Too many Dim %d ArrayDim %d.\n", dims,
            this->dimension);
    return false;
  }
  for (int dim = 0; dim < dims; ++dim) {
    auto elemStride = pattern.params[dim].stride;

    if (elemStride % curDimStride != 0) {
      DPRINTF(MLCStreamPUM,
              "[NoMask] Illegal Stride %ld Pattern %s TilePat %s.\n",
              elemStride, pattern, this->tile_pattern);
      return false;
    }

    if (elemStride != 0 && elemStride != curDimStride) {
      auto dimStride = elemStride / curDimStride;
      auto mod = starts[dim] % dimStride;
      DPRINTF(MLCStreamPUM, "[PUM] Pattern %s Dim %d Strided by %ld Mod %ld.\n",
              pattern, dim, dimStride, mod);

      auto tile = this->tile_sizes[dim];
      bool canMaskStride = true;
      if (dimStride <= tile) {
        if (tile % dimStride != 0) {
          canMaskStride = false;
        }
      } else {
        if (dimStride % tile != 0) {
          canMaskStride = false;
        }
      }
      if (!canMaskStride) {
        DPRINTF(MLCStreamPUM,
                "[PUM] Pattern %s Dim %d Stride %ld Not Align to Tile %ld.\n",
                pattern, dim, dimStride, tile);
        return false;
      }
    }

    curDimStride *= this->array_sizes[dim];
  }

  return true;
}

DataMoveCompiler::StrideMaskInfoVecT
DataMoveCompiler::turnStrideIntoMask(AffinePattern &pattern) const {

  assert(this->canTurnStrideIntoMask(pattern));

  StrideMaskInfoVecT masks;

  auto starts = pattern.getSubRegionStartToArraySize(this->array_sizes);

  auto curDimStride = 1;
  auto dims = pattern.params.size();
  for (int dim = 0; dim < dims; ++dim) {
    auto elemStride = pattern.params[dim].stride;

    if (elemStride % curDimStride != 0) {
      panic("[PUM] Illegal Stride %ld Pattern %s TilePat %s.", elemStride,
            pattern, this->tile_pattern);
    }

    if (elemStride != 0 && elemStride != curDimStride) {
      auto dimStride = elemStride / curDimStride;
      auto mod = starts[dim] % dimStride;
      DPRINTF(MLCStreamPUM, "[PUM] Pattern %s Dim %d Strided by %ld Mod %ld.\n",
              pattern, dim, dimStride, mod);

      auto tile = this->tile_sizes[dim];
      bool canMaskStride = true;
      if (dimStride <= tile) {
        if (tile % dimStride != 0) {
          canMaskStride = false;
        }
      } else {
        if (dimStride % tile != 0) {
          canMaskStride = false;
        }
      }
      if (!canMaskStride) {
        panic("[PUM] Pattern %s Dim %d Stride %ld Not Align to Tile %ld.",
              pattern, dim, dimStride, tile);
      }

      masks.emplace_back(dim, dimStride, elemStride, mod);
    }

    curDimStride *= this->array_sizes[dim];
  }

  /**
   * For the remaining missing dimension, add trip count 1.
   * This only works when we missing outer dimension.
   */
  for (int dim = dims; dim < this->dimension; ++dim) {
    DPRINTF(MLCStreamPUM,
            "[PUM] Add OuterDim to Pattern %s Dim %d Stride %ld.\n", pattern,
            dim, curDimStride);
    pattern.params.emplace_back(curDimStride, 1 /* trip */);
    curDimStride *= this->array_sizes[dim];
  }

  if (masks.empty()) {
    return masks;
  }

  // Rewrite the pattern to get rid of stride.
  DPRINTF(MLCStreamPUM, "[PUM] Rewrite Pattern %s to Remove Stride.\n",
          pattern);
  for (const auto &mask : masks) {
    auto dim = mask.dim;
    auto &p = pattern.params[dim];
    p.stride /= mask.dimStride;
    p.trip = (p.trip - 1) * mask.dimStride + 1;
    if (starts[dim] + p.trip > this->array_sizes[dim]) {
      panic("[PUM] Overflow after rewritten Dim %d NewTrip %ld.", dim, p.trip);
    }
  }
  DPRINTF(MLCStreamPUM, "[PUM] Rewritten -> %s.\n", pattern);
  return masks;
}

bool DataMoveCompiler::canCompileStreamPair(AffinePattern srcStream,
                                            AffinePattern dstStream) const {

  DPRINTF(MLCStreamPUM, "[CanPUM] Src %s -> Dst %s.\n", srcStream, dstStream);

  if (!this->canTurnStrideIntoMask(srcStream)) {
    DPRINTF(MLCStreamPUM, "[NoPUM] Can not Mask Src.\n");
    return false;
  }
  if (!this->canTurnStrideIntoMask(dstStream)) {
    DPRINTF(MLCStreamPUM, "[NoPUM] Can not Mask Dst.\n");
    return false;
  }

  auto srcMasks = this->turnStrideIntoMask(srcStream);
  auto dstMasks = this->turnStrideIntoMask(dstStream);

  if (srcMasks.size() > 1) {
    DPRINTF(MLCStreamPUM, "[NoPUM] Multi SrcMasks.\n");
    return false;
  }

  if (dstMasks.size() > 1) {
    DPRINTF(MLCStreamPUM, "[NoPUM] Multi DstMasks.\n");
    return false;
  }

  if (!isSubRegion(dstStream)) {
    DPRINTF(MLCStreamPUM, "[NoPUM] Dst Not SubRegion.\n");
    return false;
  }

  if (!isSubRegion(srcStream, true)) {
    DPRINTF(MLCStreamPUM, "[NoPUM] Src Not SubRegion with reuse.\n");
    return false;
  }

  if (srcStream.params.size() != dstStream.params.size()) {
    DPRINTF(MLCStreamPUM, "[NoPUM] Mismatch in Params Num.\n");
    return false;
  }

  auto srcTrips = srcStream.getTrips();
  auto dstTrips = dstStream.getTrips();
  for (auto i = 0; i < srcTrips.size(); ++i) {
    if (srcTrips[i] != dstTrips[i]) {
      DPRINTF(MLCStreamPUM, "[NoPUM] Mismatch in Trips.\n");
      return false;
    }
  }

  return true;
}

PUMCommandVecT
DataMoveCompiler::compileStreamPair(AffinePattern srcStream,
                                    AffinePattern dstStream) const {

  assert(this->canCompileStreamPair(srcStream, dstStream));

  auto srcMasks = this->turnStrideIntoMask(srcStream);
  auto dstMasks = this->turnStrideIntoMask(dstStream);

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
  auto reuses = this->collectReuses(srcStream);
  assert(reuses.size() <= 1);

  if (!reuses.empty() && !aligns.empty()) {
    assert(reuses.size() == aligns.size());
    for (auto i = 0; i < reuses.size(); ++i) {
      // Assert that reuse and base align are along the same dimension
      assert(reuses[i].dim == aligns[i].first);
    }
  }

  if (aligns.size() == 0) {
    // Nothing to align.
    assert(reuses.size() <= 1 && "Multi-Reuse when no align.");
    if (reuses.empty()) {
      // No need to align.
      return PUMCommandVecT();
    } else {
      // Add a fake 0 align in the reuse dimension.
      const auto &reuse = reuses.front();
      aligns.emplace_back(reuse.dim, 0 /* align distance */);
    }
  }

  /**
   * The overall compile flow:
   * 1. Compile for general data move insts to align along certain dimension.
   * 2. Mask commands the src sub-region without reuse.
   * 3. Mask commands with reuses (at the dst).
   * 4. Map commands to LLC banks.
   */

  // 1.
  DPRINTF(MLCStreamPUM, "---------------- Compile Aligns ------------\n");
  auto commands = compileAligns(aligns);

  // 2.
  DPRINTF(MLCStreamPUM, "---------------- Mask SubRegion ------------\n");
  auto reducedSrcSubRegion = removeReuseInSubRegion(srcStream);
  commands = maskCmdsBySubRegion(commands, reducedSrcSubRegion);
  if (Debug::MLCStreamPUM) {
    DPRINTF(MLCStreamPUM, "-------- After Mask SubRegion\n");
    for (const auto &c : commands) {
      DPRINTF(MLCStreamPUM, "%s", c);
    }
  }

  // 3.
  DPRINTF(MLCStreamPUM, "---------------- Mask Reuses ---------------\n");
  commands = maskCmdsByReuses(commands, reducedSrcSubRegion, reuses);
  if (Debug::MLCStreamPUM) {
    DPRINTF(MLCStreamPUM, "-------- After Mask Reuses\n");
    for (const auto &c : commands) {
      DPRINTF(MLCStreamPUM, "%s", c);
    }
  }

  // 4. Map commands to LLC configuration.
  DPRINTF(MLCStreamPUM, "---------------- Map to LLC ----------------\n");
  mapCmdsToLLC(commands);
  if (Debug::MLCStreamPUM) {
    DPRINTF(MLCStreamPUM, "-------- After Map to LLC\n");
    for (const auto &c : commands) {
      DPRINTF(MLCStreamPUM, "%s", c);
    }
  }

  // // 5. Filter out empty commands.
  // DPRINTF(MLCStreamPUM, "---------------- Filter Empty Cmd ----------\n");
  // commands = filterEmptyCmds(commands);
  // if (Debug::StreamPUM) {
  //   DPRINTF(MLCStreamPUM, "-------- After Filter Empty Cmd\n");
  //   for (const auto &c : commands) {
  //     DPRINTF(MLCStreamPUM, "%s", c);
  //   }
  // }

  return commands;
}

AffinePattern
DataMoveCompiler::removeReuseInSubRegion(const AffinePattern &pattern) const {
  assert(isSubRegion(pattern, true));
  return AffinePattern::removeReuseInSubRegion(this->array_sizes, pattern);
}

PUMCommandVecT DataMoveCompiler::compileAligns(
    const std::vector<std::pair<int64_t, int64_t>> &aligns) const {
  assert(aligns.size() == 1);
  auto dim = aligns[0].first;
  auto distance = aligns[0].second;
  return compileAlign(dim, distance);
}

template <size_t D, typename T>
AffinePatternImpl<D, T> DataMoveCompiler::constructBitlineSubRegionImpl(
    int dim, T start, T end,
    const typename AffinePatternImpl<D, T>::IntVecT &tileSizes) {

  typename AffinePatternImpl<D, T>::IntVecT starts{};
  typename AffinePatternImpl<D, T>::IntVecT trips = tileSizes;
  starts.at(dim) = start;
  trips.at(dim) = end - start;
  return AffinePatternImpl<D, T>::constructSubRegion(tileSizes, starts, trips);
}

template <size_t D, typename T>
AffinePattern DataMoveCompiler::constructBitlineSubRegionDispatch(
    int dim, int64_t start, int64_t end, const IntVecT &tileSizes) const {
  auto fixTileSizes =
      AffinePatternImpl<D, T>::getFixSizedIntVec(this->tile_sizes);
  auto fixSubRegion =
      constructBitlineSubRegionImpl<D, T>(dim, start, end, fixTileSizes);
  return getAffinePatternFromImpl(fixSubRegion);
}

AffinePattern
DataMoveCompiler::constructBitlineSubRegion(int dim, int64_t start, int64_t end,
                                            const IntVecT &tileSizes) const {

  if (this->dimension == 1) {
    return constructBitlineSubRegionDispatch<1, int64_t>(dim, start, end,
                                                         tileSizes);
  } else if (this->dimension == 2) {
    return constructBitlineSubRegionDispatch<2, int64_t>(dim, start, end,
                                                         tileSizes);
  } else if (this->dimension == 3) {
    return constructBitlineSubRegionDispatch<3, int64_t>(dim, start, end,
                                                         tileSizes);
  }

  IntVecT starts;
  IntVecT trips;
  for (auto i = 0; i < dim; ++i) {
    starts.push_back(0);
    trips.push_back(tile_sizes[i]);
  }
  starts.push_back(start);
  trips.push_back(end - start);
  for (auto i = dim + 1; i < dimension; ++i) {
    starts.push_back(0);
    trips.push_back(tile_sizes[i]);
  }
  return AffinePattern::constructSubRegion(tile_sizes, starts, trips);
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
  DPRINTF(MLCStreamPUM, "Compile Align Dim %ld Distance %ld.\n", dim, distance);
  assert(dim < dimension);
  PUMCommandVecT commands;
  commands.reserve(3);

  auto absDist = std::abs(distance);

  // Traffic within this tile.
  if (absDist < tile_sizes[dim]) {
    auto bitlines = distance;
    for (auto i = 0; i < dim; ++i) {
      bitlines *= tile_sizes[i];
    }
    commands.emplace_back();
    commands.back().type = "intra-array";
    commands.back().bitline_dist = bitlines;
    // Generate all active bitline-mask according to tile_sizes.
    commands.back().bitline_mask = AffinePattern::constructSubRegion(
        tile_sizes, AffinePattern::IntVecT(tile_sizes.size(), 0), tile_sizes);

    DPRINTF(MLCStreamPUM, "Intra-Array Cmd %s", commands.back());
  }

  /**
   * Define:
   * moveTileDist = absDist / tileSize
   * bitlineSep = absDist % tileSize
   *
   * There are basically two chuncks:
   * distance > 0:
   *   [0, tileSize-bitlineSep) -> [bitlineSep, tileSize) (+moveTileDist)
   *   [tileSize-bitlineSep, tileSize) -> [0, bitlineSep) (+moveTileDist + 1)
   *
   * distance < 0:
   *   [0, bitlineSep) -> [tileSize-bitlineSep, tileSize)  (-moveTileDist - 1)
   *   [bitlineSep, tileSize) -> [0, tileSize-bitlineSep)  (-moveTileDist)
   *
   * distance == 0:
   *   [0, tileSize)           -> 0
   *
   * And there are special cases:
   * 1. If bitlineSep == 0, we can omit one command.
   * 2. If moveTileDist == 0, we generate intra-array command.
   * 3. NOTE: When distance == 0, we still generate one intra-array command.
   */

  // Boundary case.
  auto moveTileDistNear = absDist / tile_sizes[dim];
  auto moveTileDistFar = moveTileDistNear + 1;
  auto bitlineSep = absDist % tile_sizes[dim];

  auto front_pattern =
      constructBitlineSubRegion(dim, 0, bitlineSep, tile_sizes);
  auto back_pattern =
      constructBitlineSubRegion(dim, bitlineSep, tile_sizes[dim], tile_sizes);

  auto front2_pattern = constructBitlineSubRegion(
      dim, 0, tile_sizes[dim] - bitlineSep, tile_sizes);
  auto back2_pattern = constructBitlineSubRegion(
      dim, tile_sizes[dim] - bitlineSep, tile_sizes[dim], tile_sizes);

  auto flatTileDistNear = moveTileDistNear;
  auto flatTileDistFar = moveTileDistFar;
  for (auto i = 0; i < dim; ++i) {
    flatTileDistNear *= tile_nums[i];
    flatTileDistFar *= tile_nums[i];
  }
  if (distance < 0) {
    flatTileDistNear *= -1;
    flatTileDistFar *= -1;
  }

  if (distance > 0) {

    // First chunk -> forward (May be already handled as Intra-Array.)
    if (moveTileDistNear > 0) {
      commands.emplace_back();
      commands.back().type = "inter-array";
      commands.back().tile_dist = flatTileDistNear;
      commands.back().bitline_mask = front2_pattern;
      commands.back().dst_bitline_mask = back_pattern;
      DPRINTF(MLCStreamPUM, "Inter-Array Cmd %s", commands.back());
    }

    // Second chunk -> forward (May be empty if bitlineSep == 0)
    if (bitlineSep > 0) {
      commands.emplace_back();
      commands.back().type = "inter-array";
      commands.back().tile_dist = flatTileDistFar;
      commands.back().bitline_mask = back2_pattern;
      commands.back().dst_bitline_mask = front_pattern;
      DPRINTF(MLCStreamPUM, "Inter-Array Cmd %s", commands.back());
    }
  }

  else if (distance < 0) {

    // First chunk -> backward (May be empty if bitlineSep == 0)
    if (bitlineSep > 0) {
      commands.emplace_back();
      commands.back().type = "inter-array";
      commands.back().tile_dist = flatTileDistFar;
      commands.back().bitline_mask = front_pattern;
      commands.back().dst_bitline_mask = back2_pattern;
      DPRINTF(MLCStreamPUM, "Inter-Array Cmd %s", commands.back());
    }

    // Second chunk -> backward (May be already handled as Intra-Array)
    if (moveTileDistNear > 0) {
      commands.emplace_back();
      commands.back().type = "inter-array";
      commands.back().tile_dist = flatTileDistNear;
      commands.back().bitline_mask = back_pattern;
      commands.back().dst_bitline_mask = front2_pattern;
      DPRINTF(MLCStreamPUM, "Inter-Array Cmd %s", commands.back());
    }
  }

  else {
    // distance == 0. Nothing to do.
  }

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
  auto qs = subRegion.getTrips();
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

void DataMoveCompiler::generateSubRegionMasks(
    const AffinePattern &sub_region, AffinePatternVecT &final_bitline_masks,
    AffinePatternVecT &final_tile_masks) const {

  if (this->dimension == 1) {
    this->generateSubRegionMasksDispatch<1, int64_t>(
        sub_region, final_bitline_masks, final_tile_masks);
    return;
  } else if (this->dimension == 2) {
    this->generateSubRegionMasksDispatch<2, int64_t>(
        sub_region, final_bitline_masks, final_tile_masks);
    return;
  } else if (this->dimension == 3) {
    this->generateSubRegionMasksDispatch<3, int64_t>(
        sub_region, final_bitline_masks, final_tile_masks);
    return;
  }

  MaskVecT bitline_masks;
  MaskVecT tile_masks;
  recursiveMaskSubRegionAtDim(sub_region, this->dimension - 1, bitline_masks,
                              tile_masks, final_bitline_masks,
                              final_tile_masks);
}

template <size_t D, typename T>
void DataMoveCompiler::generateSubRegionMasksDispatch(
    const AffinePattern &sub_region, AffinePatternVecT &final_bitline_masks,
    AffinePatternVecT &final_tile_masks) const {

  auto fixSubRegion = getAffinePatternImpl<D, T>(sub_region);
  auto fixArraySizes = AffinePatternImpl<D, T>::getFixSizedIntVec(array_sizes);
  auto fixTileSizes = AffinePatternImpl<D, T>::getFixSizedIntVec(tile_sizes);
  std::vector<AffinePatternImpl<D, T>> fixBitlineMasks;
  std::vector<AffinePatternImpl<D, T>> fixTileMasks;

  SubRegionMaskGenerator<D, T> gen(fixSubRegion, fixArraySizes, fixTileSizes,
                                   fixBitlineMasks, fixTileMasks);

  for (const auto &m : fixBitlineMasks) {
    final_bitline_masks.emplace_back(getAffinePatternFromImpl(m));
  }
  for (const auto &m : fixTileMasks) {
    final_tile_masks.emplace_back(getAffinePatternFromImpl(m));
  }
}

template <size_t D, typename T>
typename DataMoveCompiler::SubRegionMaskGenerator<D, T>::AffPat
DataMoveCompiler::SubRegionMaskGenerator<D, T>::mergeMasks(
    const MaskVecT &masks, const IntVecT &inner_sizes) {
  T start = 0;
  typename AffPat::ParamVecT params;
  for (auto i = 0; i < D; ++i) {
    auto dim_start = std::get<0>(masks[i]);
    auto dim_stride = std::get<1>(masks[i]);
    auto dim_trip = std::get<2>(masks[i]);
    start += dim_start * inner_sizes[i];
    params[i].stride = dim_stride * inner_sizes[i];
    params[i].trip = dim_trip;
  }
  return AffPat(start, params);
}

template <size_t D, typename T>
typename DataMoveCompiler::SubRegionMaskGenerator<D, T>::AffPat
DataMoveCompiler::SubRegionMaskGenerator<D, T>::mergeBitlineMasks(
    const MaskVecT &bitlineMasks, const IntVecT &tileSizes) {

  assert(bitlineMasks.size() == D);
  IntVecT innerTileSizes;
  for (auto i = 0; i < D; ++i) {
    auto s = AffPat::reduce_mul(tileSizes.cbegin(), tileSizes.cbegin() + i, 1);
    innerTileSizes[i] = s;
    auto trip = std::get<2>(bitlineMasks[i]);
    if (trip > tileSizes[i]) {
      panic("BitlineMask Trip %ld Overflow Tile %ld.", trip, tileSizes[i]);
    }
  }
  return mergeMasks(bitlineMasks, innerTileSizes);
}

template <size_t D, typename T>
typename DataMoveCompiler::SubRegionMaskGenerator<D, T>::AffPat
DataMoveCompiler::SubRegionMaskGenerator<D, T>::mergeTileMasks(
    const MaskVecT &tile_masks, const IntVecT &arraySizes,
    const IntVecT &tileSizes) {
  assert(tile_masks.size() == D);
  IntVecT tile_nums;
  for (auto i = 0; i < D; ++i) {
    auto a = arraySizes[i];
    auto t = tileSizes[i];
    auto s = (a + t - 1) / t;
    tile_nums[i] = s;
  }
  IntVecT inner_tile_nums;
  for (auto i = 0; i < D; ++i) {
    auto s = AffPat::reduce_mul(tile_nums.cbegin(), tile_nums.cbegin() + i, 1);
    inner_tile_nums[i] = s;
  }
  return mergeMasks(tile_masks, inner_tile_nums);
}

template <size_t D, typename T>
template <int dim, bool dummy>
void DataMoveCompiler::SubRegionMaskGenerator<D, T>::RecursiveImpl<
    dim, dummy>::impl(Generator &gen) {

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

  auto p = gen.ps[dim];
  auto q = gen.qs[dim];
  auto t = gen.tileSizes[dim];
  auto a = p / t;
  auto b = (p + t - 1) / t;
  auto c = (p + q) / t;
  auto d = (p + q + t - 1) / t;

  auto tile_p = p - a * t;
  auto tile_pq = p + q - c * t;
  if (b <= c) {
    // [P, BxTi)
    if (a < b) {
      gen.revBitlineMasks.emplace_back(tile_p, 1, t - tile_p);
      gen.revTileMasks.emplace_back(a, 1, 1);
      if (t - tile_p > t) {
        panic("BitlineMask Trip %ld Overflow Tile %ld.", t - tile_p, t);
      }
      RecursiveImpl<dim - 1, dummy>::impl(gen);
      gen.revBitlineMasks.pop_back();
      gen.revTileMasks.pop_back();
    }
    // [CxTi, P+Q)
    if (c < d) {
      gen.revBitlineMasks.emplace_back(0, 1, tile_pq);
      gen.revTileMasks.emplace_back(c, 1, 1);
      if (tile_pq > t) {
        panic("BitlineMask Trip %ld Overflow Tile %ld.", tile_pq, t);
      }
      RecursiveImpl<dim - 1, dummy>::impl(gen);
      gen.revBitlineMasks.pop_back();
      gen.revTileMasks.pop_back();
    }
    if (b < c) {
      // [BxTi, CxTi)
      gen.revBitlineMasks.emplace_back(0, 1, t);
      gen.revTileMasks.emplace_back(b, 1, c - b);
      RecursiveImpl<dim - 1, dummy>::impl(gen);
      gen.revBitlineMasks.pop_back();
      gen.revTileMasks.pop_back();
    }
  } else {
    // [P, P+Q)
    gen.revBitlineMasks.emplace_back(tile_p, 1, tile_pq - tile_p);
    gen.revTileMasks.emplace_back(a, 1, 1);
    if (tile_pq > t) {
      panic("BitlineMask %ld Overflow Tile %ld.", tile_pq, t);
    }
    RecursiveImpl<dim - 1, dummy>::impl(gen);
    gen.revBitlineMasks.pop_back();
    gen.revTileMasks.pop_back();
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
  AffinePatternVecT final_bitline_masks;
  AffinePatternVecT final_tile_masks;
  this->generateSubRegionMasks(sub_region, final_bitline_masks,
                               final_tile_masks);

  PUMCommandVecT masked_commands;
  masked_commands.reserve(commands.size() * final_bitline_masks.size());
  for (const auto &command : commands) {
    DPRINTF(MLCStreamPUM, "[MaskSubRegion] Masking CMD %s", command);
    const bool isIntraArray = command.type == "intra-array";
    for (int i = 0; i < final_bitline_masks.size(); ++i) {
      const auto &bitline_mask = final_bitline_masks.at(i);
      const auto &tile_mask = final_tile_masks.at(i);
      auto intersect =
          intersectBitlineMasks(command.bitline_mask, bitline_mask);
      DPRINTF(MLCStreamPUM,
              "[MaskSubRegion] Intersect CMD Bitline %s Mask %s = %s.\n",
              command.bitline_mask, bitline_mask, intersect);
      if (intersect.getTotalTrip() == 0) {
        // Empty intersection.
        continue;
      }
      if (isIntraArray) {
        // Check if we shift beyond bitlines.
        auto start = intersect.getSubRegionStartToArraySize(this->tile_sizes);
        auto trips = intersect.getTrips();
        auto dist = AffinePattern::getArrayPosition(this->tile_sizes,
                                                    command.bitline_dist);
        IntVecT movedStart;
        for (int dim = 0; dim < start.size(); ++dim) {
          movedStart.push_back(start[dim] + dist[dim]);
        }
        bool isEmpty = false;
        for (int dim = 0; dim < start.size(); ++dim) {
          if (movedStart[dim] + trips[dim] <= 0 ||
              movedStart[dim] >= this->tile_sizes[dim]) {
            DPRINTF(MLCStreamPUM,
                    "Skipped Empty Intra-Array Cmd at Dim %d Start %ld Dist "
                    "%ld MovedStart %ld Trip %d TileSize %ld.\n",
                    dim, start[dim], dist[dim], movedStart[dim], trips[dim],
                    this->tile_sizes[dim]);
            isEmpty = true;
            break;
          }
        }
        if (isEmpty) {
          continue;
        }
      }
      masked_commands.emplace_back(command);
      masked_commands.back().bitline_mask = intersect;
      masked_commands.back().tile_mask = tile_mask;
    }
  }

  return masked_commands;
}

/**************************************************************************
 * Mask commands by reuse.
 **************************************************************************/

DataMoveCompiler::ReuseInfoVecT
DataMoveCompiler::collectReuses(const AffinePattern &pattern) const {
  std::vector<ReuseInfoT> reuses;
  for (auto i = 0; i < dimension; ++i) {
    const auto &p = pattern.params[i];
    if (p.stride == 0) {
      // This is reuse dimension.
      reuses.emplace_back(i, p.trip);
    }
  }
  return reuses;
}

PUMCommandVecT DataMoveCompiler::maskCmdsByReuses(
    const PUMCommandVecT &commands, const AffinePattern &subRegion,
    const std::vector<ReuseInfoT> &reuses) const {

  if (reuses.empty()) {
    return commands;
  }

  assert(reuses.size() == 1 && "Cannot handle multi-dim reuse.");

  PUMCommandVecT ret;
  /**
   * NOTE: We assume that the source sub-region at the reuse dim has size 1,
   * therefore, the input should only has one type of command.
   */
  for (const auto &c : commands) {
    assert(c.type == commands.front().type &&
           "PUM] Mixed Inter/Intra-Array command with reuse.");
  }

  /**
   * 1. For inter-array command, this is trivial -- just record the reuse
   * information.
   * 2. For intra-array command, we have to check if the reuse is beyond tile
   * size, and generate extra inter-array command.
   */
  auto reuseDim = reuses.front().dim;
  auto reuseCount = reuses.front().count;
  for (const auto &cmd : commands) {
    auto c = cmd;
    c.reuse = reuses.front();
    ret.push_back(c);
  }

  if (commands.front().type == "inter-array") {
    return ret;
  }

  // This is intra-array. Check if we need to extra inter-array command.
  const auto &cmd = commands.front();
  auto srcBitlineSubRegionStart =
      cmd.bitline_mask.getSubRegionStartToArraySize(this->tile_sizes);

  auto bitlineDist =
      AffinePattern::getArrayPosition(this->tile_sizes, cmd.bitline_dist);
  auto reuseDimTileSize = this->tile_sizes.at(reuseDim);

  DPRINTF(MLCStreamPUM, "[PUMReuse] Handle Cmd %s", cmd);
  DPRINTF(MLCStreamPUM,
          "[PUMReuse] Dim %ld Count %ld BitlineStart %ld BitlineDist %ld "
          "TileSize %ld.\n",
          reuseDim, reuseCount, srcBitlineSubRegionStart.at(reuseDim),
          bitlineDist.at(reuseDim), reuseDimTileSize);
  auto dstBitlineStart =
      srcBitlineSubRegionStart.at(reuseDim) + bitlineDist.at(reuseDim);
  auto reuseDstBitline = dstBitlineStart + reuseCount;

  if (dstBitlineStart / reuseDimTileSize ==
      ((reuseDstBitline - 1) / reuseDimTileSize)) {
    // Reuse can be handled in the same tile.
    DPRINTF(MLCStreamPUM, "[PUMReuse] Reuse within the same tile.\n");
    return ret;
  }

  // Reuse across boundary of tile, we need extra inter-array command.
  auto bitlineReuseCount =
      reuseDimTileSize - (dstBitlineStart % reuseDimTileSize);
  auto extraAlignDist = bitlineReuseCount + bitlineDist.at(reuseDim);
  DPRINTF(MLCStreamPUM, "[PUMReuse] Extra Inter-Array Cmd Align %ld.\n",
          extraAlignDist);

  auto extraCmds = this->compileAlign(reuseDim, extraAlignDist);

  extraCmds = this->maskCmdsBySubRegion(extraCmds, subRegion);
  for (auto &c : extraCmds) {
    c.reuse = reuses.front();
    c.reuse.count -= bitlineReuseCount;
    ret.push_back(c);
  }

  return ret;
}

std::vector<AffinePatternVecT> DataMoveCompiler::getLLCBankSubRegions() const {
  auto tilePerLLCBank = this->llc_config.get_array_per_bank();
  auto numLLCBanks = this->llc_config.get_total_banks();
  std::vector<AffinePatternVecT> llcBankSubRegions;
  for (auto i = 0; i < numLLCBanks; ++i) {
    llcBankSubRegions.push_back(
        AffinePattern::break_continuous_range_into_canonical_sub_regions(
            this->tile_nums, i * tilePerLLCBank, tilePerLLCBank));
  }
  return llcBankSubRegions;
}

void DataMoveCompiler::mapCmdsToLLC(PUMCommandVecT &commands) const {
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

  // Specialize for some dimension.
  if (this->dimension == 1) {
    this->mapCmdsToLLCImpl<1, int64_t>(commands, cmdLLCMapper1);
    return;
  } else if (this->dimension == 2) {
    this->mapCmdsToLLCImpl<2, int64_t>(commands, cmdLLCMapper2);
    return;
  } else if (this->dimension == 3) {
    this->mapCmdsToLLCImpl<3, int64_t>(commands, cmdLLCMapper3);
    return;
  }

  // Construct the sub-region for each LLC bank.
  std::vector<AffinePatternVecT> llcBankSubRegions =
      this->getLLCBankSubRegions();

  // Process all commands.
  for (auto &command : commands) {
    mapCmdToLLC(command, llcBankSubRegions);
  }

  for (auto &command : commands) {
    if (command.type == "inter-array") {
      splitInterArrayCmdToLLC(command);
    }
  }

  return;
}

void DataMoveCompiler::mapCmdToLLC(
    PUMCommand &command,
    const std::vector<AffinePatternVecT> &llcBankSubRegions) const {

  auto numLLCBanks = llc_config.get_total_banks();

  auto needDstTilePattern = false;
  if (command.type == "inter-array" && command.hasReuse()) {
    needDstTilePattern = true;
  }

  command.llcSplitTileCmds.initialize(this->dimension);

  const auto commandTileStart = command.tile_mask.getStart();
  const auto commandTileEnd = command.tile_mask.getEnd();

  const auto tilePerLLCBank = llc_config.get_array_per_bank();

  for (auto i = 0; i < numLLCBanks; ++i) {

    /**
     * An optimization to skip the entire LLC bank if we know they have no
     * intersect.
     * ! This only works because after MaskCmdBySubRegion, the CmdTileMask
     * ! is a canonical subregion.
     */
    const auto llcTileStart = i * tilePerLLCBank;
    const auto llcTileEnd = (i + 1) * tilePerLLCBank;
    if (llcTileStart >= commandTileEnd || commandTileStart >= llcTileEnd) {
      continue;
    }

    for (const auto &llc_sub_region : llcBankSubRegions[i]) {
      auto intersect = AffinePattern::intersectSubRegions(
          tile_nums, command.tile_mask, llc_sub_region);
      if (intersect.getTotalTrip() == 0) {
        // Empty intersection.
        continue;
      }
      command.llcSplitTileCmds.addPattern(i, intersect);
    }
  }

  /**
   * Destination pattern is only set for "inter-array" commands with reuse.
   */
  if (needDstTilePattern) {

    command.llcSplitDstTileCmds.resize(numLLCBanks);

    for (auto i = 0; i < numLLCBanks; ++i) {

      auto numSubRegions = command.llcSplitTileCmds.getBankSubRegionCount(i);

      command.llcSplitDstTileCmds.resize(numSubRegions);

      for (auto j = 0; j < numSubRegions; ++j) {

        const auto &intersect = command.llcSplitTileCmds.getAffinePattern(i, j);

        auto &llcTiles = command.llcSplitDstTileCmds.at(i).at(j);
        llcTiles.fill(false);

        auto srcStartPos =
            intersect.getSubRegionStartToArraySize(this->tile_nums);
        auto trips = intersect.getTrips();
        auto tileDist =
            AffinePattern::getArrayPosition(this->tile_nums, command.tile_dist);
        IntVecT dstStartPos;
        for (int i = 0; i < srcStartPos.size(); ++i) {
          dstStartPos.push_back(srcStartPos[i] + tileDist[i]);
        }

        /**
         * Expand the dest tile pattern with reuse.
         */
        auto reuseDim = command.reuse.dim;
        auto reuseCount = command.reuse.count;
        assert(trips[reuseDim] == 1 && "ReuseDim should have trip count 1.");
        auto reuseDimTileSize = this->tile_sizes[reuseDim];
        auto reuseTileCount =
            (reuseCount + reuseDimTileSize - 1) / reuseDimTileSize;
        trips[reuseDim] = reuseTileCount;
        // Reuse should stay within the boundary.
        assert(dstStartPos[reuseDim] + reuseTileCount <=
               this->tile_nums[reuseDim]);

        auto dstTilePattern = AffinePattern::constructSubRegion(
            this->tile_nums, dstStartPos, trips);

        /**
         * Split the dest sub region to LLC banks.
         */
        for (int dstBankIdx = 0; dstBankIdx < numLLCBanks; ++dstBankIdx) {
          for (const auto &dstLLCBankSubRegion :
               llcBankSubRegions[dstBankIdx]) {
            auto dstIntersect = AffinePattern::intersectSubRegions(
                this->tile_nums, dstTilePattern, dstLLCBankSubRegion);
            if (dstIntersect.getTotalTrip() == 0) {
              continue;
            }
            llcTiles.at(dstBankIdx) = true;
          }
        }
      }
    }
  }
}

template <size_t D, typename T>
const typename DataMoveCompiler::CmdToLLCMapper<D, T>::LLCBankSubRegionsT &
DataMoveCompiler::CmdToLLCMapper<D, T>::getLLCBankSubRegionsImpl(
    const PUMHWConfiguration &llc_config, const IntVecT &tile_nums) {

  /**
   * We simply check that we have the same LLC banks here.
   * TODO: Really check that llc_config and tile_nums match.
   */
  auto numLLCBanks = llc_config.get_total_banks();
  if (this->llcBankSubRegions.size() == numLLCBanks) {
    return this->llcBankSubRegions;
  }

  auto tilePerLLCBank = llc_config.get_array_per_bank();

  auto fixedTileNums = AffinePatternImpl<D, T>::getFixSizedIntVec(tile_nums);

  this->llcBankSubRegions.reserve(numLLCBanks);
  for (auto i = 0; i < numLLCBanks; ++i) {
    this->llcBankSubRegions.push_back(
        AffinePatternImpl<D, T>::breakContnuousRangeIntoSubRegionStartAndTrips(
            fixedTileNums, i * tilePerLLCBank, tilePerLLCBank));
  }
  return this->llcBankSubRegions;
}

template <size_t D, typename T>
void DataMoveCompiler::mapCmdsToLLCImpl(PUMCommandVecT &commands,
                                        CmdToLLCMapper<D, T> &mapper) const {

  auto fixedTileNums = AffinePatternImpl<D, T>::getFixSizedIntVec(tile_nums);

  auto fixedLLCBankSubRegions =
      mapper.getLLCBankSubRegionsImpl(this->llc_config, this->tile_nums);

  for (auto &command : commands) {
    mapper.mapCmdToLLCImpl(command, fixedLLCBankSubRegions, this->llc_config,
                           fixedTileNums, this->tile_nums, this->tile_sizes);
  }

  for (auto &command : commands) {
    if (command.type == "inter-array") {
      splitInterArrayCmdToLLC(command);
    }
  }
}

template <size_t D, typename T>
void DataMoveCompiler::CmdToLLCMapper<D, T>::mapCmdToLLCImpl(
    PUMCommand &command, const LLCBankSubRegionsT &llcBankSubRegions,
    const PUMHWConfiguration &llc_config, const AffPatIntVecT &fixedTileNums,
    const IntVecT &tile_nums, const IntVecT &tile_sizes) {

  auto numLLCBanks = llc_config.get_total_banks();
  // As an optimization, we fixed number of banks for Jitter.
  assert(numLLCBanks == LLCTilePattern::NumBanks);

  auto fixedCmdTileMask = getAffinePatternImpl<D, T>(command.tile_mask);

  /**
   * Destination pattern is only set for "inter-array" commands with reuse.
   */
  auto needDstTilePattern = false;
  if (command.hasReuse() && command.type == "inter-array") {
    needDstTilePattern = true;
  }

  auto &llcTilePattern = command.llcSplitTileCmds.init<D>();

  /**
   * An optimization to skip the entire LLC bank if we know they have no
   * intersect.
   * ! This only works because after MaskCmdBySubRegion, the CmdTileMask
   * ! is a canonical subregion.
   * ! It is possible that CmdTileMask.getEnd() goes beyond the LLC tiles.
   * ! Use a std::min to guard that.
   */
  const auto commandTileStart = fixedCmdTileMask.getStart();
  const auto commandTileEnd = fixedCmdTileMask.getEnd();

  const auto tilePerLLCBank = llc_config.get_array_per_bank();

  const auto llcBankStart = commandTileStart / tilePerLLCBank;
  const auto llcBankEnd = std::min(
      numLLCBanks, (commandTileEnd + tilePerLLCBank - 1) / tilePerLLCBank);

  auto fixedInnerTileNums =
      AffinePatternImpl<D, T>::constructInnerArraySizes(fixedTileNums);

  auto cmdTileStarts =
      AffinePatternImpl<D, T>::getArrayPositionWithInnerArraySizes(
          fixedInnerTileNums, fixedCmdTileMask.start);
  const auto &cmdTileTrips = fixedCmdTileMask.getTrips();

  for (auto i = llcBankStart; i < llcBankEnd; ++i) {

    for (int j = 0, count = llcBankSubRegions[i].count; j < count; ++j) {
      const auto &llcSubRegion = llcBankSubRegions[i].subRegions.at(j);
      const auto &llcTileStarts = llcSubRegion.starts;
      const auto &llcTileTrips = llcSubRegion.trips;

      auto fixedIntersect = AffinePatternImpl<D, T>::intersectStartAndTrips(
          fixedInnerTileNums, cmdTileStarts, cmdTileTrips, llcTileStarts,
          llcTileTrips);

      if (fixedIntersect.getTotalTrip() == 0) {
        // Empty intersection.
        continue;
      }

      llcTilePattern.addPattern(i, fixedIntersect);
    }
  }

  if (!needDstTilePattern) {
    return;
  }

  command.llcSplitDstTileCmds.resize(numLLCBanks);

  for (auto i = llcBankStart; i < llcBankEnd; ++i) {

    auto numSubRegions = command.llcSplitTileCmds.getBankSubRegionCount(i);

    command.llcSplitDstTileCmds.at(i).resize(numSubRegions);

    for (auto j = 0; j < numSubRegions; ++j) {

      const auto &intersect = llcTilePattern.getAffinePatternImpl(i, j);

      // We have to explicitly write the type as LLCTilePattern.
      auto &llcDstTiles = command.llcSplitDstTileCmds[i][j];
      llcDstTiles.fill(false);

      auto dstStartPos =
          AffinePatternImpl<D, T>::getArrayPositionWithInnerArraySizes(
              fixedInnerTileNums, intersect.start + command.tile_dist);
      auto trips = intersect.getTrips();

      /**
       * Expand the dest tile pattern with reuse.
       */
      auto reuseDim = command.reuse.dim;
      auto reuseCount = command.reuse.count;
      assert(trips[reuseDim] == 1 && "ReuseDim should have trip count 1.");
      auto reuseDimTileSize = tile_sizes[reuseDim];
      auto reuseTileCount =
          (reuseCount + reuseDimTileSize - 1) / reuseDimTileSize;
      trips[reuseDim] = reuseTileCount;
      // Reuse should stay within the boundary.
      assert(dstStartPos[reuseDim] + reuseTileCount <= tile_nums[reuseDim]);

      /**
       * Split the dest sub region to LLC banks.
       */
      for (int dstBankIdx = 0; dstBankIdx < numLLCBanks; ++dstBankIdx) {

        for (int j = 0, count = llcBankSubRegions[dstBankIdx].count; j < count;
             ++j) {
          const auto &dstLLCBankSubRegionStartAndTrip =
              llcBankSubRegions[dstBankIdx].subRegions.at(j);

          auto fixedDstIntersect =
              AffinePatternImpl<D, T>::intersectStartAndTrips(
                  fixedInnerTileNums, dstStartPos, trips,
                  dstLLCBankSubRegionStartAndTrip.starts,
                  dstLLCBankSubRegionStartAndTrip.trips);

          if (fixedDstIntersect.getTotalTrip() == 0) {
            continue;
          }

          llcDstTiles.at(dstBankIdx) = true;
        }
      }
    }
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
                  int64_t tile_dist) -> PUMCommand::InterArraySplitPatternVecT {
    PUMCommand::InterArraySplitPatternVecT splits;
    auto abs_tile_dist = std::abs(tile_dist);
    if (abs_tile_dist < s * d) {
      auto m = abs_tile_dist / s;
      auto n = abs_tile_dist % s;
      if (tile_dist > 0) {
        // First part.
        if (abs_tile_dist >= s) {
          PUMCommand::InterArraySplitPattern::ParamVecT params = {{
              PUMCommand::InterArraySplitPattern::Param(1, s - n),
              PUMCommand::InterArraySplitPattern::Param(s, d - m),
          }};
          splits.emplace_back(0, params);
        }
        // Second part.
        PUMCommand::InterArraySplitPattern::ParamVecT params = {{
            PUMCommand::InterArraySplitPattern::Param(1, n),
            PUMCommand::InterArraySplitPattern::Param(s, d - m - 1),
        }};
        splits.emplace_back(s - n, params);
      } else {
        // First part.
        PUMCommand::InterArraySplitPattern::ParamVecT params = {{
            PUMCommand::InterArraySplitPattern::Param(1, n),
            PUMCommand::InterArraySplitPattern::Param(s, d - m - 1),
        }};
        splits.emplace_back((m + 1) * s, params);
        // Second part.
        if (abs_tile_dist >= s) {
          PUMCommand::InterArraySplitPattern::ParamVecT params = {{
              PUMCommand::InterArraySplitPattern::Param(1, s - n),
              PUMCommand::InterArraySplitPattern::Param(s, d - m),
          }};
          splits.emplace_back(m * s + n, params);
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

PUMCommandVecT
DataMoveCompiler::filterEmptyCmds(const PUMCommandVecT &commands) const {
  PUMCommandVecT ret;
  for (const auto &c : commands) {
    if (c.bitline_mask.getTotalTrip() == 0) {
      continue;
    }
    ret.push_back(c);
  }
  return ret;
}