#ifndef __CPU_TDG_ACCELERATOR_DYNAMIC_STREAM_SLICE_ID_VEC_HH__
#define __CPU_TDG_ACCELERATOR_DYNAMIC_STREAM_SLICE_ID_VEC_HH__

#include "DynamicStreamSliceId.hh"

#include <vector>

/**
 * A vector of DynamicStreamSliceId. Used for Ruby Slicc to multicast slices.
 */
struct DynamicStreamSliceIdVec {
  std::vector<DynamicStreamSliceId> sliceIds;
  const DynamicStreamSliceId &singleSliceId() const {
    assert(this->sliceIds.size() == 1);
    return this->sliceIds.front();
  }
  const DynamicStreamSliceId &firstSliceId() const {
    assert(!this->sliceIds.empty());
    return this->sliceIds.front();
  }
  void add(const DynamicStreamSliceId &sliceId) {
    assert(sliceId.isValid() && "Add Invalid SliceId to SliceIdVec.");
    this->sliceIds.push_back(sliceId);
  }
  void merge(const DynamicStreamSliceIdVec &sliceIds) {
    for (const auto &sliceId : sliceIds.sliceIds) {
      this->add(sliceId);
    }
  }
  void clear() { this->sliceIds.clear(); }
  bool isValid() const { return !this->sliceIds.empty(); }
};

std::ostream &operator<<(std::ostream &os,
                         const DynamicStreamSliceIdVec &slices);

#endif