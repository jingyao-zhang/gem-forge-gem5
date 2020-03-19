#include "tlb_set.hh"
namespace X86ISA {
void TLBSet::serialize(CheckpointOut &cp) const {
  // Only store the entries in use.
  uint32_t _size = this->assoc - this->freeList.size();
  SERIALIZE_SCALAR(_size);
  SERIALIZE_SCALAR(lruSeq);

  uint32_t _count = 0;
  for (const auto &entry : this->tlb) {
    if (entry.trieHandle != NULL) {
      entry.serializeSection(cp, csprintf("Entry%d", _count++));
    }
  }
}

void TLBSet::unserialize(CheckpointIn &cp) {
  // Do not allow to restore with a smaller tlb.
  uint32_t _size;
  UNSERIALIZE_SCALAR(_size);
  if (_size > this->assoc) {
    fatal("TLBSet assoc less than the one in checkpoint!");
  }

  UNSERIALIZE_SCALAR(lruSeq);

  for (uint32_t x = 0; x < _size; x++) {
    TlbEntry *newEntry = this->freeList.front();
    this->freeList.pop_front();

    newEntry->unserializeSection(cp, csprintf("Entry%d", x));
    newEntry->trieHandle = this->trie.insert(
        newEntry->vaddr, TlbEntryTrie::MaxBits - newEntry->logBytes, newEntry);
  }
}

} // namespace X86ISA