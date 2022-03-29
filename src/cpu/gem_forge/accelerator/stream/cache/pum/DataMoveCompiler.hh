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
  IntVecT tile_nums;
  IntVecT array_sizes;

  DataMoveCompiler(const PUMHWConfiguration &_llc_config,
                   const AffinePattern &_tile_pattern);

  PUMCommandVecT compile(const AffinePattern &src_stream,
                         const AffinePattern &dst_stream) const {
    return compileStreamPair(src_stream, dst_stream);
  }

  IntVecT getSubRegionStart(const AffinePattern &sub_region) const {
    // This is S1x...xSi
    return sub_region.getSubRegionStartToArraySize(array_sizes);
  }

  bool isSubRegion(const AffinePattern &pattern,
                   bool allowReuse = false) const {
    return pattern.is_canonical_sub_region_to_array_size(array_sizes,
                                                         allowReuse);
  }

  /**
   * Handle strided access as mask.
   */
  struct StrideMaskInfoT {
    const int dim;
    const int dimStride;    // Stride in this dimension.
    const int elemStride;   // Stride in number of elements.
    const int dimStrideMod; // Mod the dimension stride.
    StrideMaskInfoT(int _dim = 0, int _dimStride = 1, int _elemStride = 1,
                    int _dimStrideMod = 0)
        : dim(_dim), dimStride(_dimStride), elemStride(_elemStride),
          dimStrideMod(_dimStrideMod) {
      assert(elemStride >= dimStride);
      assert(elemStride % dimStride == 0);
      assert(dimStrideMod < dimStride);
    }
    bool hasMask() const { return this->dimStride > 1; }
  };
  using StrideMaskInfoVecT = std::vector<StrideMaskInfoT>;

  bool canTurnStrideIntoMask(const AffinePattern &pattern) const;
  StrideMaskInfoVecT turnStrideIntoMask(AffinePattern &pattern) const;

  bool canCompileStreamPair(AffinePattern srcStream,
                            AffinePattern dstStream) const;

  PUMCommandVecT compileStreamPair(AffinePattern srcStream,
                                   AffinePattern dstStream) const;

  AffinePattern removeReuseInSubRegion(const AffinePattern &pattern) const;

  /**
   * Compile data move instruction to align at certain dimension.
   */
  PUMCommandVecT
  compileAligns(const std::vector<std::pair<int64_t, int64_t>> &aligns) const;
  PUMCommandVecT compileAlign(int64_t dim, int64_t distance) const;

  /**
   * Mask commands by sub-region.
   */
  using MaskT = std::tuple<int64_t, int64_t, int64_t>;
  using MaskVecT = std::vector<MaskT>;

  AffinePattern mergeMasks(const MaskVecT &masks,
                           const IntVecT &inner_sizes) const;
  AffinePattern mergeBitlineMasks(const MaskVecT &bitline_masks) const;
  AffinePattern mergeTileMasks(const MaskVecT &tile_masks) const;
  AffinePattern intersectBitlineMasks(const AffinePattern &bitline_mask1,
                                      const AffinePattern &bitline_mask2) const;
  PUMCommandVecT maskCmdsBySubRegion(const PUMCommandVecT &commands,
                                     const AffinePattern &sub_region) const;

  void recursiveMaskSubRegionAtDim(const AffinePattern &sub_region, int64_t dim,
                                   MaskVecT &bitline_maskes,
                                   MaskVecT &tile_masks,
                                   AffinePatternVecT &final_bitline_masks,
                                   AffinePatternVecT &final_tile_masks) const;

  /**
   * Mask commands by reuses.
   */
  using ReuseInfoT = PUMCommand::ReuseInfoT;
  using ReuseInfoVecT = std::vector<ReuseInfoT>;
  ReuseInfoVecT collectReuses(const AffinePattern &pattern) const;
  PUMCommandVecT maskCmdsByReuses(const PUMCommandVecT &commands,
                                  const AffinePattern &subRegion,
                                  const std::vector<ReuseInfoT> &reuses) const;

  /**
   * Map commands to LLC.
   */
  PUMCommandVecT mapCmdsToLLC(const PUMCommandVecT &commands) const;

  void
  mapCmdToLLC(PUMCommand &command,
              const std::vector<AffinePatternVecT> &llcBankSubRegions) const;

  void splitInterArrayCmdToLLC(PUMCommand &command) const;

  /**
   * Filter out empty commands at the end..
   */
  PUMCommandVecT filterEmptyCmds(const PUMCommandVecT &commands) const;
};

#endif