#ifndef __ARCH_X86_TLB_SET_HH__
#define __ARCH_X86_TLB_SET_HH__

#include "arch/x86/pagetable.hh"
#include "base/trie.hh"

#include <list>
#include <vector>

/**
 * Implement one TLB set using the original implementation
 * of Trie and FreeList.
 */

namespace X86ISA {
class TLBSet : public Serializable {
public:
  using EntryList = std::list<TlbEntry *>;
  using TlbEntryTrie = Trie<Addr, TlbEntry>;
  TLBSet(uint32_t _assoc, uint32_t _numSetBit)
      : assoc(_assoc), numSetBit(_numSetBit), tlb(_assoc), lruSeq(0) {
    if (this->assoc == 0) {
      panic("TLBSet must have a positive assoc.\n");
    }
    if (this->numSetBit > 20) {
      panic("TLBSet has illegal numSetBits %u.\n", this->numSetBit);
    }

    for (int x = 0; x < this->assoc; x++) {
      tlb[x].trieHandle = nullptr;
      freeList.push_back(&tlb[x]);
    }
  }

  TlbEntry *insert(Addr vpn, const TlbEntry &entry) {
    // If somebody beat us to it, just use that existing entry.
    TlbEntry *newEntry = trie.lookup(vpn);
    if (newEntry) {
      assert(newEntry->vaddr == vpn);
      return newEntry;
    }

    if (freeList.empty())
      evictLRU();

    newEntry = freeList.front();
    freeList.pop_front();

    *newEntry = entry;
    newEntry->lruSeq = nextSeq();
    newEntry->vaddr = vpn;
    auto maskedBits = entry.logBytes + this->numSetBit;
    assert(maskedBits < TlbEntryTrie::MaxBits &&
           "No bits as Key for TLBEntryTrie.");
    newEntry->trieHandle =
        trie.insert(vpn, TlbEntryTrie::MaxBits - maskedBits, newEntry);
    return newEntry;
  }

  TlbEntry *lookup(Addr va, bool update_lru) {
    TlbEntry *entry = trie.lookup(va);
    if (entry && update_lru)
      entry->lruSeq = nextSeq();
    return entry;
  }

  void flushAll() {
    for (auto &entry : this->tlb) {
      if (entry.trieHandle) {
        trie.remove(entry.trieHandle);
        entry.trieHandle = nullptr;
        freeList.push_back(&entry);
      }
    }
  }

  void flushNonGlobal() {
    for (auto &entry : this->tlb) {
      if (entry.trieHandle && !entry.global) {
        trie.remove(entry.trieHandle);
        entry.trieHandle = nullptr;
        freeList.push_back(&entry);
      }
    }
  }

  void demapPage(Addr va, uint64_t asn) {
    TlbEntry *entry = trie.lookup(va);
    if (entry) {
      trie.remove(entry->trieHandle);
      entry->trieHandle = nullptr;
      freeList.push_back(entry);
    }
  }

  void serialize(CheckpointOut &cp) const override;
  void unserialize(CheckpointIn &cp) override;

private:
  void evictLRU() {
    unsigned lru = 0;
    for (auto i = 1; i < this->assoc; i++) {
      if (tlb[i].lruSeq < tlb[lru].lruSeq)
        lru = i;
    }
    assert(tlb[lru].trieHandle);
    trie.remove(tlb[lru].trieHandle);
    tlb[lru].trieHandle = nullptr;
    freeList.push_back(&tlb[lru]);
  }
  uint64_t nextSeq() { return ++lruSeq; }

  uint32_t assoc;
  uint32_t numSetBit;
  std::vector<TlbEntry> tlb;
  EntryList freeList;
  TlbEntryTrie trie;
  uint64_t lruSeq;
};
} // namespace X86ISA

#endif