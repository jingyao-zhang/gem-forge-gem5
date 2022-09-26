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
    return pattern.isSubRegionToArraySize(array_sizes, allowReuse);
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

  PERF_NOINLINE void
  generateSubRegionMasks(const AffinePattern &sub_region,
                         AffinePatternVecT &final_bitline_masks,
                         AffinePatternVecT &final_tile_masks) const;
  PERF_NOINLINE void
  recursiveMaskSubRegionAtDim(const AffinePattern &sub_region, int64_t dim,
                              MaskVecT &bitline_maskes, MaskVecT &tile_masks,
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
   * Generate TileMask for each LLC bank.
   */
  PERF_NOINLINE std::vector<AffinePatternVecT> getLLCBankSubRegions() const;

  /**
   * Map commands to LLC.
   */
  PERF_NOINLINE void mapCmdsToLLC(PUMCommandVecT &commands) const;

  PERF_NOINLINE void
  mapCmdToLLC(PUMCommand &command,
              const std::vector<AffinePatternVecT> &llcBankSubRegions) const;

  PERF_NOINLINE void splitInterArrayCmdToLLC(PUMCommand &command) const;

  /**
   * Filter out empty commands at the end..
   */
  PUMCommandVecT filterEmptyCmds(const PUMCommandVecT &commands) const;

private:
  /**
   * Optimized implementation with template.
   */

  AffinePattern constructBitlineSubRegion(int dim, int64_t start, int64_t end,
                                          const IntVecT &tileSizes) const;

  template <size_t D, typename T>
  AffinePattern
  constructBitlineSubRegionDispatch(int dim, int64_t start, int64_t end,
                                    const IntVecT &tileSizes) const;

  template <size_t D, typename T>
  static AffinePatternImpl<D, T> constructBitlineSubRegionImpl(
      int dim, T start, T end,
      const typename AffinePatternImpl<D, T>::IntVecT &tileSizes);

  template <size_t D, typename T> struct SubRegionMaskGenerator {

    using AffPat = AffinePatternImpl<D, T>;
    using AffPatVecT = std::vector<AffPat>;
    using MaskVecT = MaxVector<MaskT, D>;
    using IntVecT = typename AffPat::IntVecT;

    const AffPat &subRegion;
    const IntVecT &arraySizes;
    const IntVecT &tileSizes;

    // Array position of SubRegion start.
    IntVecT ps;
    // SubRegion trips in each dimension.
    const IntVecT &qs;

    // State during the recursive work.
    MaskVecT revBitlineMasks;
    MaskVecT revTileMasks;

    // Final results.
    AffPatVecT &retBitlineMasks;
    AffPatVecT &retTileMasks;

    SubRegionMaskGenerator(const AffPat &_subRegion, const IntVecT &_arraySizes,
                           const IntVecT &_tileSizes,
                           AffPatVecT &_retBitlineMasks,
                           AffPatVecT &_retTileMasks)
        : subRegion(_subRegion), arraySizes(_arraySizes), tileSizes(_tileSizes),
          ps(_subRegion.getArrayPosition(_arraySizes, _subRegion.start)),
          qs(_subRegion.getTrips()), retBitlineMasks(_retBitlineMasks),
          retTileMasks(_retTileMasks) {

      RecursiveImpl<D - 1, false>::impl(*this);
    }

    AffPat mergeMasks(const MaskVecT &masks, const IntVecT &inner_sizes);

    AffPat mergeBitlineMasks(const MaskVecT &bitlineMasks,
                             const IntVecT &tileSizes);
    AffPat mergeTileMasks(const MaskVecT &tile_masks, const IntVecT &arraySizes,
                          const IntVecT &tileSizes);

    template <int dim, bool dummy> struct RecursiveImpl {
      using Generator = SubRegionMaskGenerator<D, T>;
      static void impl(Generator &gen);
    };

    template <bool dummy> struct RecursiveImpl<-1, dummy> {
      using Generator = SubRegionMaskGenerator<D, T>;
      static void impl(Generator &gen) {
        /**
         * ! Don't forget to reverse the masks first.
         */
        MaskVecT bitlineMasks = gen.revBitlineMasks;
        std::reverse(bitlineMasks.begin(), bitlineMasks.end());
        MaskVecT tileMasks = gen.revTileMasks;
        std::reverse(tileMasks.begin(), tileMasks.end());
        auto merged_bitline_masks =
            gen.mergeBitlineMasks(bitlineMasks, gen.tileSizes);
        auto merged_tile_masks =
            gen.mergeTileMasks(tileMasks, gen.arraySizes, gen.tileSizes);
        gen.retBitlineMasks.push_back(merged_bitline_masks);
        gen.retTileMasks.push_back(merged_tile_masks);
        return;
      }
    };

    // PERF_NOINLINE void recursiveImpl(int64_t dim);
  };

  template <size_t D, typename T>
  void
  generateSubRegionMasksDispatch(const AffinePattern &sub_region,
                                 AffinePatternVecT &final_bitline_masks,
                                 AffinePatternVecT &final_tile_masks) const;

  template <size_t D, typename T> struct CmdToLLCMapper {
    using LLCBankSubRegionsT = std::vector<
        typename AffinePatternImpl<D, T>::ContinuousRangeSubRegions>;

    using AffPat = AffinePatternImpl<D, T>;
    using AffPatIntVecT = typename AffPat::IntVecT;

    /**
     * We can cache the LLCBankSubRegions as they are purely determined by the
     * TilePattern. As an optimization, we also record the specialize case when
     * each LLC bank is holding just one sub-region, i.e., the LLC bank tiles
     * are aligned to the tile pattern.
     *
     * For example: if the data layout is 0:1:16:2048:32:16:128:65536:64,
     * which means 2048x2048 tiled by 16x32, total 128x64 tiles.
     *
     * If each LLC bank holds 128 tiles, then each LLC bank is aligned to the
     * tile dimension, with alignments: 1x64.
     *
     * This can be used to simplify the broacast pattern when handling reuse.
     */
    LLCBankSubRegionsT llcBankSubRegions;
    bool llcBankSubRegionsAligned = false;
    AffPatIntVecT llcBankSubRegionsTileAlignments;

    PERF_NOINLINE void
    initLLCBankSubRegions(const PUMHWConfiguration &llc_config,
                          const IntVecT &tile_nums);

    PERF_NOINLINE const LLCBankSubRegionsT &
    getLLCBankSubRegionsImpl(const PUMHWConfiguration &llc_config,
                             const IntVecT &tile_nums);

    PERF_NOINLINE void
    mapCmdToLLCImpl(PUMCommand &command,
                    const LLCBankSubRegionsT &llcBankSubRegions,
                    const PUMHWConfiguration &llc_config,
                    const AffPatIntVecT &fixedTileNums,
                    const IntVecT &tile_nums, const IntVecT &tile_sizes);
  };

  /**
   * These are mutable to cache the LLCTiles.
   */
  mutable CmdToLLCMapper<1, int64_t> cmdLLCMapper1;
  mutable CmdToLLCMapper<2, int64_t> cmdLLCMapper2;
  mutable CmdToLLCMapper<3, int64_t> cmdLLCMapper3;

  template <size_t D, typename T>
  PERF_NOINLINE void mapCmdsToLLCImpl(PUMCommandVecT &commands,
                                      CmdToLLCMapper<D, T> &mapper) const;
};

#endif