#ifndef GEM_FORGE_STREAM_DEP_EDGE_HH
#define GEM_FORGE_STREAM_DEP_EDGE_HH

#include "cache/DynStreamId.hh"

#include <vector>

namespace gem5 {

class Stream;

/**
 * This remembers the dynamic dependence between streams.
 * Similar to the static StreamDepEdge, but with more information
 * to correctly align dependences.
 * Additional information:
 * 1. FromInstanceId to get the correct dynamic stream.
 * 2. Alignment to the base element index.
 * 3. Reuse count of the base element.
 */
struct DynStreamDepEdge {

  using StaticId = DynStreamId::StaticId;
  using InstanceId = DynStreamId::InstanceId;

  enum TypeE { Addr, Value, Back, Pred, Bound };
  static const char *typeToString(const TypeE &type);
  const TypeE type = Addr;
  Stream *const baseS = nullptr;
  const StaticId baseStaticId = DynStreamId::InvalidStaticStreamId;
  const InstanceId baseInstanceId = DynStreamId::InvalidInstanceId;
  const StaticId depStaticId = DynStreamId::InvalidStaticStreamId;
  const uint64_t alignBaseElement = 0;
  uint64_t baseElemReuseCnt = 1; // When the BaseS is from OuterLoop.
  uint64_t baseElemSkipCnt = 0;  // When the BaseS is from InnerLoop.
  DynStreamDepEdge(TypeE _type, Stream *_baseS, StaticId _baseStaticId,
                   InstanceId _baseInstanceId, StaticId _depStaticId,
                   uint64_t _alignBaseElement, uint64_t _reuseBaseElement)
      : type(_type), baseS(_baseS), baseStaticId(_baseStaticId),
        baseInstanceId(_baseInstanceId), depStaticId(_depStaticId),
        alignBaseElement(_alignBaseElement),
        baseElemReuseCnt(_reuseBaseElement) {}
  bool isAddrEdge() const { return this->type == TypeE::Addr; }
  bool isValueEdge() const { return this->type == TypeE::Value; }
  bool isBackEdge() const { return this->type == TypeE::Back; }
  bool isPredEdge() const { return this->type == TypeE::Pred; }
  uint64_t getBaseElemIdx(uint64_t elemIdx) const;
};
using DynStreamEdges = std::vector<DynStreamDepEdge>;

std::ostream &operator<<(std::ostream &os, const DynStreamDepEdge::TypeE &type);
std::string to_string(const DynStreamDepEdge::TypeE &type);

} // namespace gem5

#endif