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
    commands.back().src_bitline_mask = back_pattern;
    commands.back().dst_bitline_mask = front_pattern;
  } else {
    // Move backward.
    commands.back().src_bitline_mask = front_pattern;
    commands.back().dst_bitline_mask = back_pattern;
  }

  DPRINTF(StreamPUM, "Inter-Array command %s", commands.back());

  return commands;
}