#ifndef __ARCH_X86_TLB_CACHE_HH__
#define __ARCH_X86_TLB_CACHE_HH__

#include "arch/x86/tlb_set.hh"

#include <vector>

namespace X86ISA {

class TLBCache : public Serializable {
public:
  // So far I assume all inserted entry would have 4kB size.
  static constexpr uint32_t FixPageSizeBit = 12;
  TLBCache(uint32_t _size, uint32_t _assoc);

  TlbEntry *insert(Addr vpn, const TlbEntry &entry) {
    assert(entry.logBytes == FixPageSizeBit &&
           "TLBCache does not support other page size than 4kB.");
    auto setIdx = this->selectSet(vpn);
    return this->sets.at(setIdx).insert(vpn, entry);
  }
  TlbEntry *lookup(Addr va, bool update_lru) {
    auto setIdx = this->selectSet(va);
    return this->sets.at(setIdx).lookup(va, update_lru);
  }

  void flushAll() {
    for (auto &set : this->sets) {
      set.flushAll();
    }
  }

  void flushNonGlobal() {
    for (auto &set : this->sets) {
      set.flushNonGlobal();
    }
  }

  void demapPage(Addr va, uint64_t asn) {
    auto setIdx = this->selectSet(va);
    this->sets.at(setIdx).demapPage(va, asn);
  }

  void serialize(CheckpointOut &cp) const override;
  void unserialize(CheckpointIn &cp) override;

private:
  uint32_t size;
  uint32_t assoc;
  uint32_t numSet;
  uint32_t numSetBit;

  int selectSet(Addr va) {
    // So far I assume all inserted entry would have 4kB size.
    auto setIdx = bitSelect(va, FixPageSizeBit, FixPageSizeBit + numSetBit - 1);
    assert(setIdx < this->numSet && "Overflow of setIdx");
    return setIdx;
  }

  std::vector<TLBSet> sets;
};
} // namespace X86ISA

#endif