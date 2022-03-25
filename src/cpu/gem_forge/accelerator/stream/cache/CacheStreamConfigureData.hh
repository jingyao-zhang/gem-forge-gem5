#ifndef __CPU_TDG_ACCELERATOR_STREAM_CACHE_STREAM_CONFIGURE_DATA_H__
#define __CPU_TDG_ACCELERATOR_STREAM_CACHE_STREAM_CONFIGURE_DATA_H__

#include "StrandSplitInfo.hh"
#include "StreamFloatPlan.hh"
#include "cpu/gem_forge/accelerator/stream/dyn_stream.hh"

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
  CacheStreamConfigureData(const CacheStreamConfigureData &other) = delete;
  CacheStreamConfigureData &
  operator=(const CacheStreamConfigureData &other) = delete;
  CacheStreamConfigureData(CacheStreamConfigureData &&other) = delete;
  CacheStreamConfigureData &
  operator=(CacheStreamConfigureData &&other) = delete;

  Stream *stream;
  DynStreamId dynamicId;

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
  static constexpr int64_t InvalidTotalTripCount = -1;
  int64_t totalTripCount = InvalidTotalTripCount;
  int64_t getTotalTripCount() const { return this->totalTripCount; }
  bool hasTotalTripCount() const {
    return this->totalTripCount != InvalidTotalTripCount;
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
    };
    Type type;
    CacheStreamConfigureDataPtr data;
    int reuse;
    DepEdge(Type _type, const CacheStreamConfigureDataPtr &_data, int _reuse)
        : type(_type), data(_data), reuse(_reuse) {}
  };
  struct BaseEdge {
    enum Type {
      BaseOn,
    };
    Type type;
    DynStreamId dynStreamId;
    CacheStreamConfigureDataWeakPtr data;
    int reuse;
    BaseEdge(Type _type, const CacheStreamConfigureDataPtr &_data, int _reuse)
        : type(_type), dynStreamId(_data->dynamicId), data(_data),
          reuse(_reuse) {}
  };
  std::vector<DepEdge> depEdges;
  std::vector<BaseEdge> baseEdges;
  void addUsedBy(CacheStreamConfigureDataPtr &data, int reuse);
  void addSendTo(CacheStreamConfigureDataPtr &data, int reuse);
  void addBaseOn(CacheStreamConfigureDataPtr &data, int reuse);

  /**
   * StrandId and TotalStrands. Set by MLC if enabled.
   * Default to one strand.
   */
  int strandIdx = 0;
  int totalStrands = 1;
  StrandSplitInfo strandSplit;
  // The original StreamConfig before split into strands.
  CacheStreamConfigureDataPtr streamConfig = nullptr;
  bool canSplitIntoStrands() const;
  bool isSplitIntoStrands() const { return this->totalStrands > 1; }
  CacheStreamConfigureVec splitIntoStrands(const StrandSplitInfo &strandSplit);
  DynStreamFormalParamV splitLinearParam1D(const StrandSplitInfo &strandSplit,
                                           int strandIdx,
                                           const DynStreamFormalParamV &params,
                                           AddrGenCallbackPtr callback);

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

  // Set by the MLC stream, for flow control.
  int initCreditedIdx;
};
#endif
