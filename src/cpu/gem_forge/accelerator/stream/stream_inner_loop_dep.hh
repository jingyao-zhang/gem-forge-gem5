#ifndef GEM_FORGE_STREAM_INNER_LOOP_DEPENDENCE_HH
#define GEM_FORGE_STREAM_INNER_LOOP_DEPENDENCE_HH

#include "fifo_entry_idx.hh"
#include "stream_dep_edge.hh"

#include <map>
#include <vector>

namespace gem5 {

class Stream;
class StreamEngine;
class StreamElement;

class StreamInnerLoopDepTracker {
public:
  using StaticId = DynStreamId::StaticId;
  using InstanceId = DynStreamId::InstanceId;
  using StreamDepType = DynStreamDepEdge::TypeE;
  using InnerLoopBaseDynStreamMapT = std::map<StaticId, DynStreamEdges>;
  using InnerLoopBaseTypeMapT = std::map<StaticId, StreamDepType>;

  /**
   * InnerLoopDep streams to myself.
   */
  struct InnerLoopDepDynId {
    StreamEngine *se;
    Stream *S;
    DynStreamId dynId;
    InnerLoopDepDynId(StreamEngine *_se, Stream *_S, const DynStreamId &_dynId)
        : se(_se), S(_S), dynId(_dynId) {}
  };
  using InnerLoopDepDynIdVec = std::vector<InnerLoopDepDynId>;

  StreamInnerLoopDepTracker(const DynStreamId &_dynId) : dynId(_dynId) {}

  /**
   * Record and manage dynamic InnerLoopBaseStream.
   * Unlike normal StreamDepEdge, here each OuterLoopStream element depends on
   * one INSTANCE of InnerLoopStream.
   */
  void trackAsInnerLoopBase(StaticId baseStaticId, StreamDepType type);
  bool isTrackedAsInnerLoopBase(StaticId baseStaticId) const;
  DynStreamEdges &getInnerLoopBaseEdges(StaticId baseStaticId);
  const DynStreamEdges &getInnerLoopBaseEdges(StaticId baseStaticId) const;
  void pushInnerLoopBaseDynStream(DynStreamDepEdge::TypeE type, Stream *baseS,
                                  StaticId baseStaticId,
                                  InstanceId baseInstanceId,
                                  StaticId depStaticId);

  /**
   * Check that all InnerLoopBaseElems are allocated.
   */
  bool areInnerLoopBaseElemsAllocated(const FIFOEntryIdx &FIFOIdx) {
    return this->checkInnerLoopBaseElems(FIFOIdx, false);
  }
  bool areInnerLoopBaseElemsValueReady(const FIFOEntryIdx &FIFOIdx) {
    return this->checkInnerLoopBaseElems(FIFOIdx, true);
  }

  /**
   * Get all InnerLoopBaseElem
   */
  using BaseStreamElemVec =
      std::vector<std::pair<StreamDepType, StreamElement *>>;
  void getInnerLoopBaseElems(const FIFOEntryIdx &FIFOIdx,
                             BaseStreamElemVec &baseElems);
  void releaseAllInnerLoopBase();

  /**
   * Accessor to the NumInnerLoopDepS. Used to track that InnerLoopDepS
   * has correctly got the value before we can be released.
   */
  int getNumInnerLoopDepS() const { return this->innerLoopDepDynIds.size(); }
  const InnerLoopDepDynIdVec &getInnerLoopDepS() const {
    return this->innerLoopDepDynIds;
  }
  void pushInnerLoopDepS(StreamEngine *se, Stream *S,
                         const DynStreamId &dynId) {
    this->innerLoopDepDynIds.emplace_back(se, S, dynId);
  }
  void popInnerLoopDepS(const DynStreamId &dynId);
  bool tryPopInnerLoopDepS(const DynStreamId &dynId);

  // Used to identify myself.
  const DynStreamId dynId;

private:
  // InnerLoopBase.
  InnerLoopBaseDynStreamMapT innerLoopBaseEdges;
  InnerLoopBaseTypeMapT innerLoopBaseType;

  // InnerLoopDep.
  InnerLoopDepDynIdVec innerLoopDepDynIds;

  // Actually check the InnerLoopBaseS.
  bool checkInnerLoopBaseElems(const FIFOEntryIdx &FIFOIdx,
                               bool checkValueReady);
};

} // namespace gem5

#endif