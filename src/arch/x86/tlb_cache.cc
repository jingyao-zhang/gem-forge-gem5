#include "tlb_cache.hh"
namespace X86ISA {
TLBCache::TLBCache(uint32_t _size, uint32_t _assoc)
    : size(_size), assoc(_assoc), numSet(_size / _assoc) {

  assert((this->size % this->assoc == 0) &&
         "TLBSize should be a multiple of association.");
  assert(this->numSet > 0 && "TLB numSet should be positive.");
  if ((this->numSet & (this->numSet - 1)) != 0) {
    panic("TLB numSet %u should be power of 2.", this->numSet);
  }

  for (int i = 0; i < sizeof(this->numSet) * 8; ++i) {
    if (this->numSet & (1 << i)) {
      this->numSetBit = i;
      break;
    }
  }

  for (auto i = 0; i < this->numSet; ++i) {
    this->sets.emplace_back(this->assoc, this->numSetBit);
  }
}

void TLBCache::serialize(CheckpointOut &cp) const {
  SERIALIZE_SCALAR(this->size);
  SERIALIZE_SCALAR(this->assoc);
  SERIALIZE_SCALAR(this->numSet);
  uint32_t _count = 0;
  for (const auto &set : this->sets) {
    set.serializeSection(cp, csprintf("TLBSet%d", _count++));
  }
}

void TLBCache::unserialize(CheckpointIn &cp) {
  uint32_t _size;
  uint32_t _assoc;
  uint32_t _numSet;
  UNSERIALIZE_SCALAR(_size);
  UNSERIALIZE_SCALAR(_assoc);
  UNSERIALIZE_SCALAR(_numSet);
  if (_numSet != this->numSet) {
    fatal("TLBCache numSet does not match with checkpoint.");
  }

  uint32_t _count = 0;
  for (auto &set : this->sets) {
    set.unserializeSection(cp, csprintf("TLBSet%d", _count++));
  }
}
} // namespace X86ISA