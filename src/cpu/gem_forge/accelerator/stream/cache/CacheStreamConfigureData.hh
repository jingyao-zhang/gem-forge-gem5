#ifndef __CPU_TDG_ACCELERATOR_STREAM_CACHE_STREAM_CONFIGURE_DATA_H__
#define __CPU_TDG_ACCELERATOR_STREAM_CACHE_STREAM_CONFIGURE_DATA_H__

#include "StrandSplitInfo.hh"
#include "StreamFloatPlan.hh"
#include "cpu/gem_forge/accelerator/stream/dyn_stream.hh"
#include "pum/AffinePattern.hh"

#include "DynStreamId.hh"

#include "base/types.hh"

#include <memory>

class Stream;

struct CacheStreamConfigureData;
using CacheStreamConfigureDataPtr = std::shared_ptr<CacheStreamConfigureData>;
using CacheStreamConfigureDataWeakPtr = std::weak_ptr<CacheStreamConfigureData>;

using CacheStreamConfigureVec = std::vector<CacheStreamConfigureDataPtr>;

struct CacheStreamConfigureData
    : public std::enable_shared_from_this<CacheStreamConfigureData> {
public:
  CacheStreamConfigureData(Stream *_stream, const DynStreamId &_dynamicId,
                           int _elementSize,
                           const DynStreamFormalParamV &_addrGenFormalParams,
                           AddrGenCallbackPtr _addrGenCallback);

  ~CacheStreamConfigureData();
  CacheStreamConfigureData(const CacheStreamConfigureData &other) = default;
  CacheStreamConfigureData &
  operator=(const CacheStreamConfigureData &other) = default;

  CacheStreamConfigureData(CacheStreamConfigureData &&other) = delete;
  CacheStreamConfigureData &
  operator=(CacheStreamConfigureData &&other) = delete;

  Stream *stream;
  DynStreamId dynamicId;

  /**
   * Step element count of offloaded stream.
   * So far only used to implement ReductionStream for PUM.
   * By default should always be one.
   */
  int64_t stepElemCount = 1;

  int elementSize;

  /**
   * FloatPlan.
   */
  StreamFloatPlan floatPlan;

  // NOTE: Line address here.
  Addr initVAddr;
  Addr initPAddr;
  bool initPAddrValid = false;

  // Size of the MLC buffer.
  int mlcBufferNumSlices = 32;

  /**
   * Uside to indicate a stream that only needs to be prefeteched into the LLC
   * and NOT written back to MLC.
   */
  bool isPUMPrefetch = false;

  /**
   * Used to only offload the indirect stream,
   * e.g. a[b[i]], when b[i] is marked as PseudoOffload,
   * both a[] and b[] will be offloaded, but LLC will only send back data
   * of a[]. Core requests of b[] will be treated as normal requests.
   */
  bool isPseudoOffload = false;

  /**
   * We require range-sync on this stream.
   * Some range-sync stream does not need range-commit, e.g. DirectLoadStream
   * without IndirectDependent.
   */
  bool rangeSync = false;
  bool rangeCommit = false;

  DynStreamFormalParamV addrGenFormalParams;
  AddrGenCallbackPtr addrGenCallback;
  DynStreamFormalParamV predFormalParams;
  ExecFuncPtr predCallback;

  /**
   * TotalTripCount for offloaded streams.
   * ! Support reuse at MLC, LLC streams may be cut.
   */
  static constexpr int64_t InvalidTripCount = DynStream::InvalidTripCount;
  int64_t totalTripCount = InvalidTripCount;
  int64_t getTotalTripCount() const { return this->totalTripCount; }
  bool hasTotalTripCount() const {
    return this->totalTripCount != InvalidTripCount;
  }
  int64_t innerTripCount = InvalidTripCount;
  int64_t getInnerTripCount() const { return this->innerTripCount; }
  bool hasInnerTripCount() const {
    return this->innerTripCount != InvalidTripCount;
  }

  /**
   * Remember that this stream is short and has been cutted by MLC SE.
   */
  bool hasBeenCuttedByMLC = false;

  bool isPredicated = false;
  bool isPredicatedTrue = false;
  DynStreamId predicateStreamId;

  // For StoreFunc and LoadFunc.
  DynStreamFormalParamV storeFormalParams;
  ExecFuncPtr storeCallback;
  DynStreamFormalParamV loadFormalParams;
  ExecFuncPtr loadCallback;

  // For LoopBoundFunc.
  // Break when loopBoundCallback() == Ret.
  DynStreamFormalParamV loopBoundFormalParams;
  ExecFuncPtr loopBoundCallback;
  bool loopBoundRet;

  StreamValue reductionInitValue;
  /**
   * Whether the core need the final value.
   */
  bool finalValueNeededByCore = false;

  /**
   * Whether this is a pointer chase stream.
   * TODO: This should be included in the future Pattern structure
   * TODO: when introducing execution-driven simulation.
   */
  bool isPointerChase;

  /**
   * Whether this stream should be sliced according to cache lines.
   * This is used by SlicedDynStream to determine if it should merge continous
   * stream elements in the same cache line.
   *
   * By default this is enabled, and used for slicing affine streams. However,
   * it is also used to explicitly disable slicing for streams in outer loop.
   * Otherwise, we may have deadlock as the OuterLoopStream slice contains
   * future elements.
   *
   * A typical example is Gaussian elmination:
   * for i = 0 : M
   *   B[i] ...
   *   for j = 0 : N
   *     A[i][j] ...
   *
   * Here if B[i] is sliced, we have deadlocks.
   */
  bool shouldBeSlicedToCacheLines = true;

  /**
   * NOTE: Only valid for indirect streams.
   * Sometimes the dependence between the indirect stream and the base stream
   * has a difference of one iteration, e.g. pointer chase base stream + an
   * indirect stream loading the value.
   */
  bool isOneIterationBehind;

  /**
   * This represents a distributed StreamDepGraph.
   * NOTE: Keep the graph acyclic to avoid circular dependence for shared_ptr.
   */
  struct DepEdge {
    enum Type {
      UsedBy,
      SendTo,
      PUMSendTo,
    };
    Type type;
    CacheStreamConfigureDataPtr data;
    int reuse;
    int skip;
    DepEdge(Type _type, const CacheStreamConfigureDataPtr &_data, int _reuse,
            int _skip)
        : type(_type), data(_data), reuse(_reuse), skip(_skip) {}

    /**
     * Fields for PUMSendTo relationship.
     * Notice that the receiver is handled as PUM, not stream.
     */
    AffinePattern broadcastPat;
    AffinePattern recvPat;
    AffinePattern recvTile;
  };
  struct BaseEdge {
    enum Type {
      BaseOn,
    };
    Type type;
    DynStreamId dynStreamId;
    CacheStreamConfigureDataWeakPtr data;
    int reuse;
    int skip;
    BaseEdge(Type _type, const CacheStreamConfigureDataPtr &_data, int _reuse,
             int _skip)
        : type(_type), dynStreamId(_data->dynamicId), data(_data),
          reuse(_reuse), skip(_skip) {}
  };
  std::vector<DepEdge> depEdges;
  std::vector<BaseEdge> baseEdges;
  void clearEdges() {
    this->baseEdges.clear();
    this->depEdges.clear();
  }
  void addUsedBy(CacheStreamConfigureDataPtr &data);
  void addSendTo(CacheStreamConfigureDataPtr &data, int reuse, int skip);
  void addPUMSendTo(const CacheStreamConfigureDataPtr &data,
                    const AffinePattern &broadcastPat,
                    const AffinePattern &recvPat,
                    const AffinePattern &recvTile);
  void addBaseOn(CacheStreamConfigureDataPtr &data, int reuse, int skip);
  static uint64_t convertBaseToDepElemIdx(uint64_t baseElemIdx, int reuse,
                                          int skip);
  static uint64_t convertDepToBaseElemIdx(uint64_t depElemIdx, int reuse,
                                          int skip);

  /**
   * @brief Hint to MLCStrandManager to avoid split outer dimension.
   * When set to positive value, the MLCStrandManager avoids split outer loops.
   * For example: a stream with pattern:
   *   0 : 1 : 512 : 512 : 512
   * With hintNoStrandSplitOuterTripCount set to 512, the MLCStrandManager will
   * split the stream with:
   *  initOffset = 0,
   *  interleave = TotalTrip/HintNoSplit/Cores = 512 * 512 / 512 / 64 = 8
   *  strands = Cores = 64
   * Essentially, each strand will process 8 columnes of the array.
   *
   * Default value is 0, so that MLCStrandManager can choose how to split the
   * stream freely.
   */
  int64_t hintNoStrandSplitOuterTripCount = 0;

  /**
   * @brief Override the default latency. This is only an AdHoc solution to
   * split out Reduction, and is only correct for latency. The stats of number
   * of instructions executed is still the same.
   * TODO: Really split the reduction part in the compiler.
   */
  int overrideComputeLatency = -1;

  /**
   * StrandId and TotalStrands. Set by MLC if enabled.
   * Default to one strand.
   */
  int strandIdx = 0;
  int totalStrands = 1;
  StrandSplitInfo strandSplit;
  // The original StreamConfig before split into strands.
  CacheStreamConfigureDataPtr streamConfig = nullptr;
  bool isStrandConfig() const { return streamConfig != nullptr; }
  bool isSplitIntoStrands() const { return this->totalStrands > 1; }
  DynStreamFormalParamV splitLinearParam1D(const StrandSplitInfo &strandSplit,
                                           int strandIdx);
  DynStreamFormalParamV splitAffinePatternAtDim(int splitDim, int strandIdx,
                                                int totalStrands);

  /**
   * Get the StrandId from StreamElemIdx.
   * Must be called on the original StreamConfig.
   * Used to find out the receiving strand for sending strand.
   */
  DynStrandId getStrandIdFromStreamElemIdx(uint64_t streamElemIdx) const;
  uint64_t getStrandElemIdxFromStreamElemIdx(uint64_t streamElemIdx) const;

  /**
   * Exchange between the StreamElemIdx and StrandElemIdx.
   */
  uint64_t getStreamElemIdxFromStrandElemIdx(uint64_t strandElemIdx) const;
  uint64_t getStreamElemIdxFromStrandElemIdx(const DynStrandId &strandId,
                                             uint64_t strandElemIdx) const;

  // Set by the MLC stream, for flow control.
  int initCreditedIdx;

  /**
   * Information to coordinate streams with PUMEngine.
   * The stream need to wait for the PUMEngine whenever it reachs PUMElemPerSync
   * elements, and each time the PUMEngine need to finish one round of
   * computation befores the stream could continue.
   */
  static constexpr int64_t InvalidPUMContextId = -1;
  int64_t pumContextId = InvalidPUMContextId;
  int64_t pumElemPerSync = 0;     // 0 Means never need to sync.
  bool waitPUMRoundStart = false; // Default to wait on Round Complete.
  bool needSyncWithPUMEngine() const {
    return this->pumContextId != InvalidContextID && pumElemPerSync > 0;
  }
  int64_t waitForPUMRounds(int64_t elemIdx) const {
    return elemIdx / pumElemPerSync;
  }
  uint64_t getFirstPUMRoundElemIdx(int64_t roundIdx) const {
    assert(roundIdx >= 0);
    return roundIdx * pumElemPerSync;
  }
};
#endif
