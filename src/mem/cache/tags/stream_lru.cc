
#include "mem/cache/tags/stream_lru.hh"

#include "debug/CacheRepl.hh"
#include "mem/cache/base.hh"

namespace gem5 {

StreamLRU::StreamLRU(const Params *p) : BaseSetAssoc(p) {}

CacheBlk *StreamLRU::accessBlock(Addr addr, bool is_secure, Cycles &lat) {
  CacheBlk *blk = BaseSetAssoc::accessBlock(addr, is_secure, lat);

  if (blk != nullptr) {
    // move this block to head of the MRU list
    sets[blk->set].moveToHead(blk);
    DPRINTF(CacheRepl, "set %x: moving blk %x (%s) to MRU\n", blk->set,
            regenerateBlkAddr(blk->tag, blk->set), is_secure ? "s" : "ns");
  }

  return blk;
}

CacheBlk *StreamLRU::findVictim(Addr addr) {
  int set = extractSet(addr);
  // grab a replacement candidate
  BlkType *blk = nullptr;
  for (int i = assoc - 1; i >= 0; i--) {
    BlkType *b = sets[set].blks[i];
    if (b->way < allocAssoc) {
      blk = b;
      break;
    }
  }
  assert(!blk || blk->way < allocAssoc);

  if (blk && blk->isValid()) {
    DPRINTF(CacheRepl, "set %x: selecting blk %x for replacement\n", set,
            regenerateBlkAddr(blk->tag, set));
  }

  return blk;
}

void StreamLRU::insertBlock(PacketPtr pkt, BlkType *blk) {
  BaseSetAssoc::insertBlock(pkt, blk);

  int set = extractSet(pkt->getAddr());
  sets[set].moveToHead(blk);
}

void StreamLRU::insertBlockLRU(PacketPtr pkt, BlkType *blk) {
  BaseSetAssoc::insertBlock(pkt, blk);

  int set = extractSet(pkt->getAddr());
  sets[set].moveToTail(blk);
}

void StreamLRU::invalidate(CacheBlk *blk) {
  BaseSetAssoc::invalidate(blk);

  // should be evicted before valid blocks
  int set = blk->set;
  sets[set].moveToTail(blk);
}

StreamLRU *StreamLRUParams::create() { return new StreamLRU(this); }
} // namespace gem5

