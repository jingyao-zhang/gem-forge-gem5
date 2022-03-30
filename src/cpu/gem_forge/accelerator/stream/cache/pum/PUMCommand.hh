#ifndef __CPU_GEM_FORGE_PUM_COMMAND_HH__
#define __CPU_GEM_FORGE_PUM_COMMAND_HH__

#include "AffinePattern.hh"
#include "cpu/op_class.hh"

#include "../DynStreamId.hh"

class PUMCommand {
public:
  std::string type = "none";
  AffinePattern bitline_mask;
  AffinePattern dst_bitline_mask; // Only for inter-array command.
  AffinePattern tile_mask;
  int64_t tile_dist = 0;
  int64_t bitline_dist = 0;

  bool isReduction = false; // This command is reduction operation.

  /**
   * Extra reuse information.
   */
  struct ReuseInfoT {
    int64_t dim = 0;
    int64_t count = 1;
    ReuseInfoT(int64_t _dim, int64_t _count) : dim(_dim), count(_count) {}
    ReuseInfoT() : dim(0), count(1) {}
  };
  ReuseInfoT reuse;
  bool hasReuse() const { return reuse.count > 1; }

  std::vector<AffinePatternVecT> inter_array_splits;

  struct LLCTileMask {
    AffinePattern srcTilePattern;
    AffinePattern dstTilePattern;
    std::vector<AffinePatternVecT> dstSplitTilePatterns;
  };

  std::vector<std::vector<LLCTileMask>> llcSplitTileCmds;

  int wordline_bits;
  // Only valid for compute command.
  OpClass opClass = IntAluOp;

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

  std::string to_string(int llcBankIdx = -1) const;
};

std::ostream &operator<<(std::ostream &os, const PUMCommand &command);

using PUMCommandVecT = std::vector<PUMCommand>;

#endif