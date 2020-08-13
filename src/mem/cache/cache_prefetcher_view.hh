#ifndef __MEM_CACHE_CACHE_PREFETCHER_VIEW_HH__
#define __MEM_CACHE_CACHE_PREFETCHER_VIEW_HH__

/**
 * @file Defines an interface for prefetcher to access the cache.
 */

#include "cpu/thread_context.hh"

class CachePrefetcherView {
public:
  /**
   * Query block size of a cache.
   * @return  The block size
   */
  virtual unsigned getBlockSize() const = 0;

  virtual bool inCache(Addr addr, bool is_secure) const = 0;
  virtual bool inMissQueue(Addr addr, bool is_secure) const = 0;
  virtual bool hasBeenPrefetched(Addr addr, bool is_secure) const = 0;
  virtual bool coalesce() const = 0;
  virtual ProbeManager *getCacheProbeManager() = 0;
  virtual ThreadContext *getThreadContext(ContextID contextId) = 0;
};

#endif