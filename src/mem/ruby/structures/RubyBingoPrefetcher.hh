
#ifndef __MEM_RUBY_STRUCTURES_BINGO_PREFETCHER_HH__
#define __MEM_RUBY_STRUCTURES_BINGO_PREFETCHER_HH__

// Implements Power 4 like prefetching

#include <bitset>

#include "base/statistics.hh"
#include "mem/ruby/common/Address.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"
#include "mem/ruby/slicc_interface/RubyRequest.hh"
#include "mem/ruby/system/RubySystem.hh"
#include "params/RubyBingoPrefetcher.hh"
#include "sim/sim_object.hh"
#include "sim/system.hh"

namespace L1D_PREF {
class Bingo;
}

class RubyBingoPrefetcher : public SimObject {
public:
  typedef RubyBingoPrefetcherParams Params;
  RubyBingoPrefetcher(const Params *p);
  ~RubyBingoPrefetcher();

  void setController(AbstractController *_ctrl) { m_controller = _ctrl; }

  void regStats();

  bool isEnabled() const { return this->enabled; }

  /**
   * Oberseve that the cache served a request.
   * @param address: the physical line address.
   * @param pc: pc causing this request.
   * @param hit: whether this request has hit in cache.
   * @param type: Type of the request.
   */
  void observeReq(Addr address, Addr pc, bool hit, const RubyRequestType &type);

  /**
   * Observe that the cache filled in a line.
   * @param evictedAddr: the address of the evicted line.
   */
  void observeEvict(Addr evictedAddr);

  /**
   * API used by the bingo prefetcher to issue a prefetching request.
   * Note so far we always prefetch into the controller, so pfFillLevel is
   * ignored.
   * @return 1 if prefetching request issued to controller.
   */
  int prefetchLine(Addr pc, Addr baseAddr, Addr pfAddr, int pfFillLevel);

private:
  bool enabled;
  AbstractController *m_controller;

  const Addr m_page_shift;

  //! Count of accesses to the prefetcher
  Stats::Scalar numMissObserved;
  //! Count of prefetch streams allocated
  Stats::Scalar numAllocatedStreams;
  //! Count of prefetch requests made
  Stats::Scalar numPrefetchRequested;
  //! Count of successful prefetches
  Stats::Scalar numHits;
  //! Count of partial successful prefetches
  Stats::Scalar numPartialHits;
  //! Count of pages crossed
  Stats::Scalar numPagesCrossed;
  //! Count of misses incurred for blocks that were prefetched
  Stats::Scalar numMissedPrefetchedBlocks;

  Addr pageAddress(Addr addr) const;

  // The actual prefetcher.
  std::unique_ptr<L1D_PREF::Bingo> bingo;
};

#endif // __MEM_RUBY_STRUCTURES_PREFETCHER_HH__
