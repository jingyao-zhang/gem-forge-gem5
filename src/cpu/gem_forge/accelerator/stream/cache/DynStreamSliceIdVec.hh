#ifndef __CPU_GEM_FORGE_DYN_STREAM_SLICE_ID_VEC_HH__
#define __CPU_GEM_FORGE_DYN_STREAM_SLICE_ID_VEC_HH__

#include "DynStreamSliceId.hh"

#include <vector>

/**
 * A vector of DynStreamSliceId. Used for Ruby Slicc to multicast slices.
 */
struct DynStreamSliceIdVec {
  std::vector<DynStreamSliceId> sliceIds;
  const DynStreamSliceId &singleSliceId() const {
    assert(this->sliceIds.size() == 1);
    return this->sliceIds.front();
  }
  const DynStreamSliceId &firstSliceId() const {
    assert(!this->sliceIds.empty());
    return this->sliceIds.front();
  }
  void add(const DynStreamSliceId &sliceId) {
    assert(sliceId.isValid() && "Add Invalid SliceId to SliceIdVec.");
    this->sliceIds.push_back(sliceId);
  }
  void merge(const DynStreamSliceIdVec &sliceIds) {
    for (const auto &sliceId : sliceIds.sliceIds) {
      this->add(sliceId);
    }
  }
  void clear() { this->sliceIds.clear(); }
  bool isValid() const { return !this->sliceIds.empty(); }
};

std::ostream &operator<<(std::ostream &os, const DynStreamSliceIdVec &slices);

#endif