#ifndef __MEM_CACHE_TAGS_STREAM_LRU_HH__
#define __MEM_CACHE_TAGS_STREAM_LRU_HH__

#include "base_set_assoc.hh"
#include "params/StreamLRU.hh"

namespace gem5 {

class StreamLRU : public BaseSetAssoc {
public:
  using Params = StreamLRUParams;

  StreamLRU(const Params *p);

  ~StreamLRU() {}

  CacheBlk *accessBlock(Addr addr, bool is_secure, Cycles &lat);
  CacheBlk *findVictim(Addr addr);
  void insertBlock(PacketPtr pkt, BlkType *blk);
  void insertBlockLRU(PacketPtr pkt, BlkType *blk);
  void invalidate(CacheBlk *blk);
};

} // namespace gem5

#endif