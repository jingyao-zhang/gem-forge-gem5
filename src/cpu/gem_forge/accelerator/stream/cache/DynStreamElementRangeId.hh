#ifndef __CPU_GEM_FORGE_DYN_STREAM_ELEMENT_RANGE_ID_HH__
#define __CPU_GEM_FORGE_DYN_STREAM_ELEMENT_RANGE_ID_HH__

#include "DynStreamId.hh"

#include "base/types.hh"

/**
 * This is a simple helper structure that represents a range of elements
 * from [lhsElementIdx, rhsElementIdx].
 */

struct DynStreamElementRangeId {
  DynStreamId streamId;
  uint64_t lhsElementIdx;
  uint64_t rhsElementIdx;
  DynStreamElementRangeId() : streamId(), lhsElementIdx(0), rhsElementIdx(0) {}

  bool isValid() const {
    return !(this->lhsElementIdx == 0 && this->rhsElementIdx == 0);
  }
  void clear() {
    this->streamId = DynStreamId();
    this->lhsElementIdx = 0;
    this->rhsElementIdx = 0;
  }
  const uint64_t &getLHSElementIdx() const { return this->lhsElementIdx; }
  uint64_t &getLHSElementIdx() { return this->lhsElementIdx; }
  uint64_t getNumElements() const {
    return this->rhsElementIdx - this->lhsElementIdx;
  }
  bool contains(uint64_t elementIdx) const {
    return elementIdx >= this->lhsElementIdx &&
           elementIdx < this->rhsElementIdx;
  }

  bool operator==(const DynStreamElementRangeId &other) const {
    return this->streamId == other.streamId &&
           this->lhsElementIdx == other.lhsElementIdx &&
           this->rhsElementIdx == other.rhsElementIdx;
  }

  bool operator!=(const DynStreamElementRangeId &other) const {
    return !(this->operator==(other));
  }
};

std::ostream &operator<<(std::ostream &os, const DynStreamElementRangeId &id);

struct DynStreamElementRangeIdHasher {
  std::size_t operator()(const DynStreamElementRangeId &key) const {
    return (DynStreamIdHasher()(key.streamId)) ^
           std::hash<uint64_t>()(key.lhsElementIdx) ^
           std::hash<uint64_t>()(key.rhsElementIdx);
  }
};

#endif