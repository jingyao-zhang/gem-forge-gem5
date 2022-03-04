#ifndef __CPU_GEM_FORGE_PUM_COMMAND_HH__
#define __CPU_GEM_FORGE_PUM_COMMAND_HH__

#include "AffinePattern.hh"

#include "../DynStreamId.hh"

class PUMCommand {
public:
  std::string type = "none";
  AffinePattern bitline_mask;
  AffinePattern src_bitline_mask;
  AffinePattern dst_bitline_mask;
  AffinePattern tile_mask;
  int64_t tile_dist = 0;
  int64_t bitline_dist = 0;

  std::vector<AffinePatternVecT> inter_array_splits;
  std::vector<AffinePatternVecT> llc_commands;

  int wordline_bits;

  /**
   * These are meta info used for logging and debugging.
   */

  DynStreamId dynStreamId;

  std::string srcRegion = "none";
  AffinePattern srcAccessPattern;
  AffinePattern srcMapPattern;
  std::string dstRegion = "none";
  AffinePattern dstAccessPattern;
  AffinePattern dstMapPattern;

  std::string to_string() const;
};

std::ostream &operator<<(std::ostream &os, const PUMCommand &command);

using PUMCommandVecT = std::vector<PUMCommand>;

#endif