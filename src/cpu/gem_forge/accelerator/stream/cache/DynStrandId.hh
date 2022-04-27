
#ifndef __CPU_GEM_FORGE_DYN_STRAND_ID_HH__
#define __CPU_GEM_FORGE_DYN_STRAND_ID_HH__

#include "DynStreamId.hh"

/**
 * Represent a strand of a DynStreamId. Used to auto parallelize streams across
 * banks.
 */
struct DynStrandId {

  using StrandIndex = int32_t;
  static constexpr StrandIndex InvalidStrandIdx = -1;
  static constexpr StrandIndex DefaultFirstStrandIdx = 0;
  static constexpr StrandIndex DefaultTotalStrands = 1;

  DynStreamId dynStreamId;
  StrandIndex strandIdx = InvalidStrandIdx;
  StrandIndex totalStrands = 0;

  bool isValid() const { return strandIdx != InvalidStrandIdx; }

  DynStrandId() = default;

  explicit DynStrandId(const DynStreamId &_dynStreamId,
                       StrandIndex _strandIdx = DefaultFirstStrandIdx,
                       StrandIndex _totalStrands = DefaultTotalStrands)
      : dynStreamId(_dynStreamId), strandIdx(_strandIdx),
        totalStrands(_totalStrands) {}

  DynStrandId(const DynStrandId &other)
      : dynStreamId(other.dynStreamId), strandIdx(other.strandIdx),
        totalStrands(other.totalStrands) {}
  DynStrandId &operator=(const DynStrandId &other) {
    this->dynStreamId = other.dynStreamId;
    this->strandIdx = other.strandIdx;
    this->totalStrands = other.totalStrands;
    return *this;
  }

  bool isSameStaticStream(const DynStrandId &other) const {
    return this->dynStreamId.isSameStaticStream(other.dynStreamId);
  }
  bool isSameDynStream(const DynStrandId &other) const {
    return this->dynStreamId == other.dynStreamId;
  }
  bool operator==(const DynStrandId &other) const {
    return this->dynStreamId == other.dynStreamId &&
           this->strandIdx == other.strandIdx;
  }
  bool operator!=(const DynStrandId &other) const {
    return !(this->operator==(other));
  }
  bool operator<(const DynStrandId &other) const {
    if (this->isSameDynStream(other)) {
      return this->strandIdx < other.strandIdx;
    } else {
      return this->dynStreamId < other.dynStreamId;
    }
  }
};

std::ostream &operator<<(std::ostream &os, const DynStrandId &streamId);

std::string to_string(const DynStrandId &streamId);

struct DynStrandIdHasher {
  std::size_t operator()(const DynStrandId &key) const {
    return DynStreamIdHasher()(key.dynStreamId) ^
           std::hash<DynStrandId::StrandIndex>()(key.strandIdx);
  }
};

#endif