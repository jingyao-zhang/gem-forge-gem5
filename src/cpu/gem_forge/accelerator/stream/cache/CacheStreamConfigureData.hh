#ifndef __CPU_TDG_ACCELERATOR_STREAM_CACHE_STREAM_CONFIGURE_DATA_H__
#define __CPU_TDG_ACCELERATOR_STREAM_CACHE_STREAM_CONFIGURE_DATA_H__

#include "cpu/gem_forge/accelerator/stream/dyn_stream.hh"

#include "DynamicStreamId.hh"

#include "base/types.hh"

#include <memory>

class Stream;

struct CacheStreamConfigureData;
using CacheStreamConfigureDataPtr = std::shared_ptr<CacheStreamConfigureData>;
using CacheStreamConfigureDataWeakPtr = std::weak_ptr<CacheStreamConfigureData>;

struct CacheStreamConfigureData
    : public std::enable_shared_from_this<CacheStreamConfigureData> {
public:
  CacheStreamConfigureData(
      Stream *_stream, const DynamicStreamId &_dynamicId, int _elementSize,
      const std::vector<DynamicStreamFormalParam> &_addrGenFormalParams,
      AddrGenCallbackPtr _addrGenCallback);
  CacheStreamConfigureData(const CacheStreamConfigureData &other) = delete;
  CacheStreamConfigureData &
  operator=(const CacheStreamConfigureData &other) = delete;
  CacheStreamConfigureData(CacheStreamConfigureData &&other) = delete;
  CacheStreamConfigureData &
  operator=(CacheStreamConfigureData &&other) = delete;

  Stream *stream;
  DynamicStreamId dynamicId;
  int elementSize;

  // NOTE: Line address here.
  Addr initVAddr;
  Addr initPAddr;
  bool initPAddrValid = false;

  /**
   * Used to only offload the indirect stream,
   * e.g. a[b[i]], when b[i] is marked as PseudoOffload,
   * both a[] and b[] will be offloaded, but LLC will only send back data
   * of a[]. Core requests of b[] will be treated as normal requests.
   */
  bool isPseudoOffload = false;

  /**
   * We require range-sync on this stream.
   */
  bool rangeSync = false;

  DynamicStreamFormalParamV addrGenFormalParams;
  AddrGenCallbackPtr addrGenCallback;
  DynamicStreamFormalParamV predFormalParams;
  ExecFuncPtr predCallback;
  int64_t totalTripCount = -1;
  bool isPredicated = false;
  bool isPredicatedTrue = false;
  DynamicStreamId predicateStreamId;

  // For StoreFunc and LoadFunc.
  DynamicStreamFormalParamV storeFormalParams;
  ExecFuncPtr storeCallback;
  DynamicStreamFormalParamV loadFormalParams;
  ExecFuncPtr loadCallback;

  StreamValue reductionInitValue;

  /**
   * Whether this is a pointer chase stream.
   * TODO: This should be included in the future Pattern structure
   * TODO: when introducing execution-driven simulation.
   */
  bool isPointerChase;

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
    DepEdge(Type _type, const CacheStreamConfigureDataPtr &_data)
        : type(_type), data(_data) {}
  };
  struct BaseEdge {
    enum Type {
      BaseOn,
    };
    Type type;
    CacheStreamConfigureDataWeakPtr data;
    BaseEdge(Type _type, const CacheStreamConfigureDataWeakPtr &_data)
        : type(_type), data(_data) {}
  };
  std::vector<DepEdge> depEdges;
  std::vector<BaseEdge> baseEdges;
  void addUsedBy(CacheStreamConfigureDataPtr &data) {
    this->depEdges.emplace_back(DepEdge::Type::UsedBy, data);
    data->baseEdges.emplace_back(BaseEdge::Type::BaseOn,
                                 this->shared_from_this());
  }
  void addSendTo(CacheStreamConfigureDataPtr &data);
  void addBaseOn(CacheStreamConfigureDataPtr &data) {
    this->baseEdges.emplace_back(BaseEdge::Type::BaseOn, data);
  }

  // Set by the MLC stream, for flow control.
  int initCreditedIdx;
};

using CacheStreamConfigureVec = std::vector<CacheStreamConfigureDataPtr>;

#endif
